#include <algorithm>
#include <vector>
#include <cmath>
#include <X11/Xutil.h>
#include <fstream>
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <json-c/json.h>
#include <sys/wait.h>
#include <sys/prctl.h>
#include <poll.h>
#include <fcntl.h>
#include <unistd.h>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
using Json = std::unique_ptr<json_object, decltype(&json_object_put)>;

std::string field(json_object *obj, const char *key) {
    json_object *v = nullptr;
    if (!json_object_object_get_ex(obj, key, &v) || !v) return "-";
    return json_object_get_string(v);
}

class Receiver {
public:
    std::string dir, status = "Ready";
    pid_t pid = -1;
    bool automatic = false, rtlAgc = false, modeAC = false;
    int gain = 496;
    explicit Receiver(std::string path) : dir(std::move(path)) {}
    void start() {
        if (pid > 0) return;
        fs::remove(dir + "/aircraft.json");
        pid_t parent = getpid();
        pid = fork();
        if (pid == 0) {
            prctl(PR_SET_PDEATHSIG, SIGTERM);
            if (getppid() != parent) _exit(1);
            int log = open((dir + "/receiver.log").c_str(), O_WRONLY|O_CREAT|O_TRUNC, 0600);
            if (log < 0) _exit(1);
            dup2(log, STDERR_FILENO);
            dup2(log, STDOUT_FILENO);
            close(log);
            std::string gainArg = automatic ? "-10" : std::to_string(gain / 10.0);
            std::vector<const char *> args = {"dump1090-mutability", "--quiet", "--gain", gainArg.c_str(),
                "--write-json", dir.c_str(), "--write-json-every", "1", "--net",
                "--net-bind-address", "127.0.0.1", "--net-bo-port", "31001", "--net-ro-port", "0",
                "--net-ri-port", "0", "--net-bi-port", "0", "--net-sbs-port", "30003"};
            if (rtlAgc) args.push_back("--enable-agc");
            if (modeAC) args.push_back("--modeac");
            args.push_back(nullptr);
            execvp(args.front(), const_cast<char *const *>(args.data()));
            _exit(127);
        }
        status = pid < 0 ? "Could not start decoder" : "Starting receiver";
    }
    void check() {
        if (pid > 0 && waitpid(pid, nullptr, WNOHANG) == pid) {
            pid = -1;
            status = "Receiver exited - check dongle / local ports";
            std::ifstream log(dir + "/receiver.log");
            std::cerr << log.rdbuf();
        }
    }
    void stop() {
        if (pid <= 0) return;
        kill(pid, SIGTERM);
        waitpid(pid, nullptr, 0);
        pid = -1;
        status = "Stopped";
    }
    ~Receiver() { stop(); std::error_code ec; fs::remove_all(dir, ec); }
};

int main(int argc, char **argv) {
    Display *d = XOpenDisplay(nullptr);
    if (!d) { std::cerr << "X11 display unavailable\n"; return 1; }
    char path[] = "/tmp/rtl1090x-XXXXXX";
    if (!mkdtemp(path)) return 1;
    Receiver rx(path);
    constexpr int width = 540;
    int height = 650;
    Window w = XCreateSimpleWindow(d, DefaultRootWindow(d), 0, 0, width, height, 0, 0, 0x080935);
    XStoreName(d, w, "RTL1090X - Mode S receiver");
    XSizeHints hints{};
    hints.flags = PMinSize | PMaxSize;
    hints.min_width = hints.max_width = width;
    hints.min_height = 430; hints.max_height = 1600;
    XSetWMNormalHints(d,w,&hints);
    Atom quit = XInternAtom(d,"WM_DELETE_WINDOW",False);
    XSetWMProtocols(d,w,&quit,1);
    XSelectInput(d,w,ExposureMask|ButtonPressMask|KeyPressMask|StructureNotifyMask);
    XMapWindow(d,w);
    GC gc = XCreateGC(d,w,0,nullptr);
    auto font = XLoadQueryFont(d,"8x13");
    if (font) XSetFont(d,gc,font->fid);
    Pixmap canvas = 0;
    auto box = [&](int x,int y,int ww,int hh,unsigned color) {
        XSetForeground(d,gc,color); XFillRectangle(d,canvas,gc,x,y,ww,hh);
    };
    auto label = [&](int x,int y,unsigned color,const std::string &s) {
        XSetForeground(d,gc,color); XDrawString(d,canvas,gc,x,y,s.c_str(),s.size());
    };
    auto bevel = [&](int x,int y,int ww,int hh,unsigned fill,bool inset=false) {
        box(x,y,ww,hh,inset ? 0x545459 : 0xe8e7dd);
        box(x+2,y+2,ww-2,hh-2,inset ? 0xe8e7dd : 0x545459);
        box(x+2,y+2,ww-4,hh-4,fill);
    };
    auto button = [&](int x,int y,int ww,std::string title,bool on=false,unsigned fill=0xc9c8be) {
        bevel(x,y,ww,28,fill,on);
        label(x+(ww-int(title.size())*8)/2,y+19,on ? 0x00552c : 0x171720,title);
    };
    // Square phosphor characters, drawn independently of the installed desktop fonts.
    const std::vector<std::vector<int>> glyphs = {
        {4,12,4,4,4,4,14},{14,17,17,15,1,1,14},{14,17,17,17,17,17,14},
        {0,0,0,0,0,0,4},{17,27,21,17,17,17,17},{17,17,17,31,17,17,17},
        {0,0,31,2,4,8,31}};
    bool running = true, list = true, info = false;
    int offset = 0, rate = 0;
    long previous = 0;
    auto last = Clock::now();
    if (argc==2 && std::string(argv[1])=="--start") rx.start();
    while (running) {
        while (XPending(d)) {
            XEvent e; XNextEvent(d,&e);
            if (e.type==ClientMessage && static_cast<Atom>(e.xclient.data.l[0])==quit) running=false;
            if (e.type==ConfigureNotify) height=e.xconfigure.height;
            bool toggle=e.type==KeyPress && XLookupKeysym(&e.xkey,0)==XK_space;
            if (e.type==ButtonPress) {
                auto b=e.xbutton;
                if (b.button==Button4) offset=std::max(0,offset-3);
                if (b.button==Button5) offset+=3;
                if (b.button==Button1) {
                    if (b.y>=147 && b.y<175) {
                        if (b.x>=440 && b.x<528) toggle=true;
                        if (b.x>=205 && b.x<279) list=!list;
                        if (b.x>=295 && b.x<369) info=!info;
                        if (b.x>=14 && b.x<191 && rx.pid<=0) rx.modeAC=b.x>=102;
                    }
                    if (b.y>=93 && b.y<121 && rx.pid<=0) {
                        if (b.x>=14 && b.x<120) rx.automatic=!rx.automatic;
                        if (b.x>=132 && b.x<=322) {
                            rx.automatic=false;
                            rx.gain=(b.x-132)*496/190;
                        }
                        if (b.x>=432 && b.x<526) rx.rtlAgc=!rx.rtlAgc;
                    }
                }
            }
            if (toggle) {
                if (rx.pid>0) rx.stop(); else rx.start();
                previous=0; rate=0; last=Clock::now();
            }
        }
        rx.check();
        Json data(rx.pid>0 ? json_object_from_file((rx.dir+"/aircraft.json").c_str()) : nullptr,json_object_put);
        json_object *rows=nullptr,*messages=nullptr;
        if (data) {
            json_object_object_get_ex(data.get(),"aircraft",&rows);
            json_object_object_get_ex(data.get(),"messages",&messages);
            rx.status="Receiving";
        }
        int count=rows ? json_object_array_length(rows) : 0;
        long total=json_object_get_int64(messages);
        auto now=Clock::now();
        double elapsed=std::chrono::duration<double>(now-last).count();
        if (elapsed>=1 && data) {
            rate=std::max(0L,std::lround((total-previous)/elapsed));
            previous=total; last=now;
        }
        canvas=XCreatePixmap(d,w,width,height,DefaultDepth(d,DefaultScreen(d)));
        box(0,0,width,height,0x080935);
        bevel(10,10,520,60,0x010600,true);
        int x=30;
        for (char c : std::string("1090.000 MHz")) {
            if (c!=' ') {
                const auto &bitmap=glyphs[std::string("190.MHz").find(c)];
                for (int row=0;row<7;++row)
                    for (int col=0;col<5;++col)
                        if (bitmap[row] & (16>>col)) box(x+col*6,20+row*6,6,6,0x39f52c);
            }
            x+=40;
        }
        label(12,87,0xe0e0d7,"Automatic/Manual gain");
        button(14,93,106,"Tuner AGC",rx.automatic);
        bevel(132,104,191,8,0x77777d,true);
        bevel(132+rx.gain*184/496,96,8,22,0xcdcdc4);
        char gain[18]; snprintf(gain,sizeof(gain),"%.1f dB",rx.gain/10.0);
        label(338,112,0xe1e1dd,rx.automatic ? "Auto" : gain);
        button(432,93,94,"RTL AGC",rx.rtlAgc);
        label(14,140,0xe0e0d7,"Mode");
        button(14,147,84,"MODE-S",!rx.modeAC);
        button(102,147,89,"+MODE-AC",rx.modeAC);
        button(205,147,74,"List",list);
        button(295,147,74,"Info",info);
        button(440,147,88,rx.pid>0 ? "STOP" : "START",false,0xf1de35);
        bevel(9,188,522,height-253,0x212224,true);
        box(12,191,516,22,0x10145b);
        label(16,207,0xc8d0fa,"ICAO    C/S       ALT ft  V/S    GS   TRK  SSR   MSGS  AGE");
        if (info) {
            label(17,237,0x7ae597,rx.status);
            label(17,258,0x7ae597,"Gain and mode controls are available when stopped.");
            label(17,279,0x7ae597,"Local Beast TCP 31001 / BaseStation TCP 30003");
        } else if (list) {
            int visible=std::max(1,(height-288)/18);
            offset=std::clamp(offset,0,std::max(0,count-visible));
            for (int i=offset;i<count && i<offset+visible;++i) {
                auto row=json_object_array_get_idx(rows,i);
                char line[100];
                snprintf(line,sizeof(line),"%-7.7s %-9.9s %-7.7s %-6.6s %-4.4s %-4.4s %-5.5s %-5.5s %.5s",
                    field(row,"hex").c_str(),field(row,"flight").c_str(),field(row,"altitude").c_str(),
                    field(row,"vert_rate").c_str(),field(row,"speed").c_str(),field(row,"track").c_str(),
                    field(row,"squawk").c_str(),field(row,"messages").c_str(),field(row,"seen").c_str());
                label(16,232+(i-offset)*18,0x62d991,line);
            }
            if (!count) label(16,233,0x87aa98,rx.pid>0 ? "Waiting for decoded aircraft..." : rx.status);
        }
        int thresholds[]={1,10,20,40,80,120};
        for (int i=0;i<6;++i) bevel(15+32*i,height-51,27,16,data && rate>=thresholds[i]?0x23aeff:0x575766,true);
        label(224,height-38,0xd5d8e2,rx.status);
        bevel(0,height-26,width,26,0xc9c8be);
        label(10,height-9,0x151520,std::to_string(data ? rate : 0)+"/sec     TCP: 31001     A/C: "+std::to_string(count)+"     MSG: "+std::to_string(total));
        XCopyArea(d,canvas,w,gc,0,0,width,height,0,0);
        XFreePixmap(d,canvas);
        XFlush(d);
        pollfd fd{ConnectionNumber(d),POLLIN,0};
        poll(&fd,1,500);
    }
    XCloseDisplay(d);
}
