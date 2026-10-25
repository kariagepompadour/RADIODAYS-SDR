// Host harness: runs the firmware's own analogue-radio drawing code (extract.inc, taken verbatim from the .ino)
// on a fake 240x135 LCD and writes PPM images.  Fonts: the real M5GFX GLCD and FreeSerif tables.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>
#include <set>
#include <algorithm>
#define PROGMEM
#define M_PI_F 3.14159265f
template<class T> T constrain(T v, T lo, T hi){ return v<lo?lo:(v>hi?hi:v); }
template<class T> T max(T a, T b){ return a>b?a:b; }
template<class T> T min(T a, T b){ return a<b?a:b; }
struct PrintRec{ int x,y,w; std::string s; bool onLcd; }; static std::vector<PrintRec> printLog; static bool recOn=false;
struct GFXglyph { uint32_t bitmapOffset; uint8_t width,height; uint8_t xAdvance; int8_t xOffset,yOffset; };
struct GFXfont { uint8_t* bitmap; GFXglyph* glyph; uint16_t first,last; uint8_t yAdvance; };
#include "FreeSans9pt7b.h"
#include "FreeSansBold9pt7b.h"
#include "glcdfont.h"
enum class textdatum_t { top_left, top_center, top_right, middle_left, middle_center, middle_right, bottom_left, bottom_center, bottom_right };
namespace fonts { extern GFXfont& dummy; }
struct FontRef { const GFXfont* g; };
namespace fonts { static FontRef FreeSans9pt7b_{&::FreeSans9pt7b}, FreeSansBold9pt7b_{&::FreeSansBold9pt7b};
  static FontRef& FreeSans9pt7b=FreeSans9pt7b_; static FontRef& FreeSansBold9pt7b=FreeSansBold9pt7b_; }
namespace fonts { static FontRef Font0{nullptr}; }
static uint16_t rgb565(int r,int g,int b){ return ((r&0xF8)<<8)|((g&0xFC)<<3)|(b>>3); }
struct Gfx {
  int w,h; std::vector<uint16_t> px; int depth=16; std::vector<uint8_t> px8;
  const GFXfont* font=nullptr; float size=1; uint16_t fg=0xFFFF,bg=0; bool hasBg=false; textdatum_t datum=textdatum_t::top_left; int pad=0; int cx=0,cy=0;
  Gfx(int W=0,int H=0):w(W),h(H),px(W*H,0){}
  Gfx(void*):w(0),h(0){}
  bool createSprite(int W,int H){ w=W;h=H; if(depth==8) px8.assign(W*H,0); else px.assign(W*H,0); return true; }
  void deleteSprite(){}
  void setColorDepth(int d){ depth=d; }
  void put(int x,int y,uint16_t c){ if(x>=0&&y>=0&&x<w&&y<h){ if(depth==8) px8[y*w+x]=(uint8_t)c; else px[y*w+x]=c; } }
  void fillRect(int x,int y,int W,int H,uint16_t c){ for(int j=0;j<H;j++)for(int i=0;i<W;i++)put(x+i,y+j,c); }
  void fillScreen(uint16_t c){ fillRect(0,0,w,h,c); }
  void drawFastHLine(int x,int y,int W,uint16_t c){ fillRect(x,y,W,1,c); }
  void drawFastVLine(int x,int y,int H,uint16_t c){ fillRect(x,y,1,H,c); }
  void drawRect(int x,int y,int W,int H,uint16_t c){ drawFastHLine(x,y,W,c);drawFastHLine(x,y+H-1,W,c);drawFastVLine(x,y,H,c);drawFastVLine(x+W-1,y,H,c); }
  void drawRoundRect(int x,int y,int W,int H,int r,uint16_t c){ drawFastHLine(x+r,y,W-2*r,c);drawFastHLine(x+r,y+H-1,W-2*r,c);drawFastVLine(x,y+r,H-2*r,c);drawFastVLine(x+W-1,y+r,H-2*r,c); put(x+1,y+1,c);put(x+W-2,y+1,c);put(x+1,y+H-2,c);put(x+W-2,y+H-2,c); }
  void drawLine(int x0,int y0,int x1,int y1,uint16_t c){ int dx=abs(x1-x0),sx=x0<x1?1:-1,dy=-abs(y1-y0),sy=y0<y1?1:-1,e=dx+dy; for(;;){ put(x0,y0,c); if(x0==x1&&y0==y1)break; int e2=2*e; if(e2>=dy){e+=dy;x0+=sx;} if(e2<=dx){e+=dx;y0+=sy;} } }
  void fillCircle(int cx,int cy,int r,uint16_t c){ for(int j=-r;j<=r;j++)for(int i=-r;i<=r;i++) if(i*i+j*j<=r*r) put(cx+i,cy+j,c); }
  static float edge(int ax,int ay,int bx,int by,int px,int py){ return (float)((px-ax)*(by-ay)-(py-ay)*(bx-ax)); }
  void fillTriangle(int x0,int y0,int x1,int y1,int x2,int y2,uint16_t c){ int minx=std::min({x0,x1,x2}),maxx=std::max({x0,x1,x2}),miny=std::min({y0,y1,y2}),maxy=std::max({y0,y1,y2}); for(int y=miny;y<=maxy;y++)for(int x=minx;x<=maxx;x++){ float a=edge(x0,y0,x1,y1,x,y),b=edge(x1,y1,x2,y2,x,y),d=edge(x2,y2,x0,y0,x,y); if((a>=0&&b>=0&&d>=0)||(a<=0&&b<=0&&d<=0)) put(x,y,c);} }
  void fillArc(int cx,int cy,int r0,int r1,float a0,float a1,uint16_t c){ for(int y=cy-r1-1;y<=cy+r1+1;y++)for(int x=cx-r1-1;x<=cx+r1+1;x++){ float dx=x-cx,dy=y-cy,d=sqrtf(dx*dx+dy*dy); if(d<r0||d>r1+0.5f) continue; float a=atan2f(dy,dx)*180.f/M_PI; if(a<0)a+=360; float s=fmodf(a0,360),e=fmodf(a1,360); if(s<0)s+=360; if(e<0)e+=360; bool in= s<=e ? (a>=s&&a<=e):(a>=s||a<=e); if(in) put(x,y,c);} }
  void fillRectAlpha(int x,int y,int W,int H,uint8_t a,uint16_t c){ for(int j=0;j<H;j++)for(int i=0;i<W;i++){ int xx=x+i,yy=y+j; if(xx<0||yy<0||xx>=w||yy>=h) continue; uint16_t o=px[yy*w+xx]; auto mix=[&](int sh,int m){ int oc=(o>>sh)&m, nc=(c>>sh)&m; return (oc*(255-a)+nc*a)/255; }; px[yy*w+xx]=(mix(11,31)<<11)|(mix(5,63)<<5)|mix(0,31);} }
  void setFont(const FontRef* f){ font=f->g; }
  void setFont(const FontRef& f){ font=f.g; }
  // M5GFX: setTextFont(1) does nothing while a GFX (free) font is active; this is what hid the bug on the PC before
  void setTextFont(int f){ if(f==1 && font) return; font=nullptr; }
  void setTextSize(float s){ size=s; }
  void drawPixel(int x,int y,uint16_t c){ put(x,y,c); }
  void startWrite(){} void endWrite(){}
  void setTextColor(uint16_t f){ fg=f; hasBg=false; }
  void setTextColor(uint16_t f,uint16_t b){ fg=f; bg=b; hasBg=true; }
  void setTextDatum(textdatum_t d){ datum=d; }
  void setTextPadding(int p){ pad=p; }
  void setCursor(int x,int y){ cx=x;cy=y; }
  void print(const char* s){ if(recOn) printLog.push_back({cx,cy,w,s,w==240&&h==135}); drawStr(s,cx,cy,textdatum_t::top_left,true); }
  // glyph drawing
  int glcdW(const char*s){ return (int)strlen(s)*6*size; }
  void drawStr(const char* s,int x,int y,textdatum_t d,bool useCursor=false){
    int W,H,asc=0; int isz=(int)size; if(isz<1) isz=1;
    if(!font){ W=(int)strlen(s)*6*isz; H=8*isz; }
    else { int ab=0,bb=0; for(int c=0;c<=font->last-font->first;c++){ const GFXglyph&g=font->glyph[c]; int a=-g.yOffset; ab=std::max(ab,a); bb=std::max(bb,(int)g.height-a);} asc=ab; H=(int)((ab+bb)*size); W=0; for(const char*p=s;*p;p++){ int c=(unsigned char)*p-font->first; if(c>=0&&c<=font->last-font->first) W+=(int)(font->glyph[c].xAdvance*size); } }
    int x0=x,y0=y;
    switch(d){ case textdatum_t::top_center: case textdatum_t::middle_center: case textdatum_t::bottom_center: x0=x-W/2; break; case textdatum_t::top_right: case textdatum_t::middle_right: case textdatum_t::bottom_right: x0=x-W; break; default: break; }
    if(d==textdatum_t::middle_left||d==textdatum_t::middle_center||d==textdatum_t::middle_right) y0=y-H/2;
    if(d==textdatum_t::bottom_left||d==textdatum_t::bottom_center||d==textdatum_t::bottom_right) y0=y-H;
    if(hasBg){ int pw=std::max(W,pad); int px0 = (d==textdatum_t::top_left||d==textdatum_t::middle_left)? x0 : x0-(pw-W)/2; if(d==textdatum_t::top_right||d==textdatum_t::middle_right) px0=x0-(pw-W); fillRect(px0,y0,pw,H,bg); }
    int cxp=x0;
    for(const char*p=s;*p;p++){
      if(!font){ int c=(unsigned char)*p; for(int col=0;col<5;col++){ uint8_t line=::font[c*5+col]; for(int row=0;row<7;row++) if(line>>row&1) fillRect(cxp+col*isz,y0+row*isz,isz,isz,fg); } cxp+=6*isz; }
      else { int c=(unsigned char)*p-font->first; if(c<0||c>font->last-font->first) continue; const GFXglyph&g=font->glyph[c]; const uint8_t* bm=font->bitmap+g.bitmapOffset;
        int xo=(int)(g.xOffset*size); int ytop=y0+(int)((asc+g.yOffset)*size);
        for(int yy=0;yy<g.height;yy++){ int ya=(int)(yy*size), yb=(int)((yy+1)*size); int fh=(yb==ya)?1:(yb-ya);
          int xx=0; while(xx<g.width){ int bit=yy*g.width+xx; bool on=bm[bit>>3]&(0x80>>(bit&7)); if(!on){ xx++; continue; } int run=0; while(xx+run<g.width){ int b2=yy*g.width+xx+run; if(!(bm[b2>>3]&(0x80>>(b2&7)))) break; run++; }
            int xa=(int)(xx*size), xb=(int)((xx+run)*size); fillRect(cxp+xo+xa,ytop+ya,std::max(1,xb-xa),fh,fg); xx+=run; } }
        cxp+=(int)(g.xAdvance*size); }
    }
  }
  void drawString(const char*s,int x,int y){ if(recOn) printLog.push_back({x,y,w,s,w==240&&h==135}); drawStr(s,x,y,datum); }
  uint8_t* getBuffer(){ return depth==8 ? px8.data() : (uint8_t*)px.data(); }
  void drawCircle(int cx,int cy,int r,uint16_t c){ for(int j=-r-1;j<=r+1;j++)for(int i=-r-1;i<=r+1;i++){ float d=sqrtf(i*i+j*j); if(fabsf(d-r)<0.6f) put(cx+i,cy+j,c);} }
  static uint16_t c332to565(uint8_t v){ int r=(v>>5)&7,g=(v>>2)&7,b=v&3; return rgb565(r*255/7,g*255/7,b*255/3); }
  void pushSprite(Gfx*dst,int x,int y){ for(int j=0;j<h;j++)for(int i=0;i<w;i++) dst->put(x+i,y+j, depth==8 ? c332to565(px8[j*w+i]) : px[j*w+i]); }
};
typedef Gfx M5Canvas;
static Gfx lcdObj(240,135);
#define lcd lcdObj
struct String { std::string s; String(){} String(char c):s(1,c){} String(const char* c):s(c){} const char* c_str()const{return s.c_str();} bool isEmpty()const{return s.empty();} };
static uint32_t fakeMs=100000; static uint32_t millis(){ fakeMs+=40; return fakeMs; }
struct Lock {};

static constexpr float FREQ_MIN=10.0f, FREQ_MAX=30000.0f;
static constexpr uint16_t COL_BG=0, COL_TEXT=0xFFFF, COL_DIM=0x8410, COL_AMBER=0xFD20, COL_OK=0x07E0, COL_WARN=0xFFE0, COL_BAD=0xF800;
enum WifiState { W_IDLE, W_TRY, W_UP, W_WAIT }; static WifiState wifiState=W_UP;
struct SS{ std::string s; SS substring(int a,int b){ return SS{s.substr(a,b-a)}; } const char* c_str(){return s.c_str();} };
static int wifiStatusVal=3;
struct WiFiC { int status(){ return wifiStatusVal; } SS SSID(){ return SS{"HomeNet-2G"}; } int RSSI(){ return -58; } } WiFi; static const int WL_CONNECTED=3;
struct SavedNet { char ssid[33]; char pass[65]; }; static SavedNet saved[3]; static int savedCount=1; static int wifiOrder[3]={0}, wifiOrderPos=0;
static float wantFreq=10000, STEPS_[4]={0.1f,1.0f,5.0f,10.0f}; static const float* STEPS=STEPS_; static int wantMode=0,wantVolume=70,stepIdx=1; static bool wantMuted=false;
static char clockDate[12]="SAT 10/25", clockTime[12]="07:58 JST";
static const char* const MODE_LABEL[]={"AM","USB","LSB"};
struct Remote { char scan[8]="off"; char scanMsg[12]=""; float scanExcess=0; float freq=0; uint32_t scanDoneMs=0; bool ok=true; char link[12]="connected"; char server[24]="Osaka JP"; char error[40]=""; float rssi=-80,s=7,outLevel=0.05f,inLevel=0; int sOver=0; int serverIndex=0, serverCount=4; char output[12]="mac"; bool audioClient=false; uint32_t audioSent=0,audioDropped=0,audioDisc=0; } rem;
static char netStatus[40]="PC: OK";
static Gfx dialSpriteObj, meterSpriteObj; static Gfx* dialSprite=&dialSpriteObj; static Gfx* meterSprite=&meterSpriteObj;
#include "RadioDays_AudioProto.h"
#include "RadioDays_Playout.h"
static volatile int wantOutput=0; static volatile bool auWanted=false, auConn=false, auNoMem=false; static volatile uint8_t auNetState=2; static volatile float auLevel=0;
static rdaudio::Playout auPlayout; static rdaudio::NetStats auNet;
struct StreamRing { size_t bytes=6000; } auRingObj; static StreamRing* auRing=&auRingObj; static size_t xStreamBufferBytesAvailable(StreamRing* r){ return r->bytes; }
struct WiFiMore {}; 
struct EspC { unsigned getFreeHeap(){return 123456;} unsigned getMinFreeHeap(){return 98000;} } ESP; static unsigned heap_caps_get_largest_free_block(int){return 65536;}
#define MALLOC_CAP_8BIT 0
static String macIP("192.168.1.23"); static uint32_t diagDrawMs=0;
#define BUILD_TAG "SDR-1.10"
#include <new>
#include "RadioDays_Types.h"
static Screen screen=SC_RADIO; static bool screenDirty=true;
static uint32_t verMode=0, lastRadioSaveMs=0; static bool radioDirty=false;
namespace lgfx { static uint8_t color332(int r,int g,int b){ return (uint8_t)((r&0xE0)|((g&0xE0)>>3)|(b>>6)); } }
#include "globe_land.h"
struct SrvInfo { char shortName[22]; char name[44]; float lat, lon; bool hasPos; };
static SrvInfo srvs[32]; static int srvCount=0; static volatile bool srvWant=false; static uint32_t srvRev=0, statusRev=0;
static int wantServer=0; static uint32_t verServer=0; static bool macManual=false;
struct FakeSerial { template<class... A> void printf(const char*,A...){} void println(const char*){} } Serial;
static bool analogReady=true; static volatile bool tuneAuto=false; static uint32_t autoHintUntil=0; static int wantScanDir=0; static uint32_t verScan=0; static uint32_t verFreq=1; static bool synced=true;
// the other screens and handlers are not under test here: stubs
static bool resultOk=false; static bool gKeyG=false, gKeyEsc=false, gKeyD=false;
static void globeOpen(); static void globeClose(); static void setScreen(Screen s);
static void handleRadio(const KeyInput&, const KnobInput&){ if(gKeyG){ gKeyG=false; globeOpen(); } if(gKeyD){ gKeyD=false; setScreen(SC_DIAG); } }
static void handleGlobe(const KeyInput&, const KnobInput&){ if(gKeyEsc){ gKeyEsc=false; globeClose(); } }
static void handleWifiList(const KeyInput&, const KnobInput&){} static void handleWifiPass(const KeyInput&){} static void handleWifiJoin(const KeyInput&){} static void handleWifiSaved(const KeyInput&, const KnobInput&){}
static bool anyKey(const KeyInput&){ return false; } static bool hasChar(const KeyInput&, char){ return false; } static void leaveWifiSetup(){}
static bool acceptFreq(const String&){ return true; } static bool acceptMacIP(const String&){ return true; }
static void handleEntry(const KeyInput&, bool, size_t, bool(*)(const String&)){}
static void drawWifiList(){} static void drawWifiPass(){} static void drawWifiJoin(){} static void drawWifiSaved(){} static void drawWifiResult(){}
static void drawEntry(const char*, const char*, const char*){}
static void drawHelp();
#include "extract.inc"
static void drawHelp(){ header("KEYS"); }
static void ppm(const char* path){ FILE* f=fopen(path,"wb"); const int Z=4; fprintf(f,"P6\n%d %d\n255\n",240*Z,135*Z); for(int y=0;y<135*Z;y++)for(int x=0;x<240*Z;x++){ uint16_t c=lcdObj.px[(y/Z)*240+x/Z]; fputc(((c>>11)&31)*255/31,f);fputc(((c>>5)&63)*255/63,f);fputc((c&31)*255/31,f);} fclose(f); }
static void ppm1(const char* path){ FILE* f=fopen(path,"wb"); fprintf(f,"P6\n240 135\n255\n"); for(int i=0;i<240*135;i++){ uint16_t c=lcdObj.px[i]; fputc(((c>>11)&31)*255/31,f);fputc(((c>>5)&63)*255/63,f);fputc((c&31)*255/31,f);} fclose(f); }
static void frame(const char* name, bool full=true){ for(int i=0;i<30;i++) drawRadioAnalog(i==0&&full); char p[96]; snprintf(p,96,"%s/%s.ppm",getenv("OUT"),name); ppm(p); snprintf(p,96,"%s/%s_1x.ppm",getenv("OUT"),name); ppm1(p); }

// ---- consistency tests on the firmware's own drawing code
static std::vector<uint16_t> region(int x,int y,int w,int h){ std::vector<uint16_t> v; for(int j=0;j<h;j++)for(int i=0;i<w;i++) v.push_back(lcdObj.px[(y+j)*240+x+i]); return v; }
static int fails=0;
static void check(bool ok,const char* what){ printf("%s  %s\n", ok?"OK  ":"FAIL", what); if(!ok) fails++; }
static void freshState(float f,int step,int vol,bool mute){ wantFreq=f; stepIdx=step; wantVolume=vol; wantMuted=mute; drawRadioAnalog(true); }
static int framesToSettle(float from,float to,int step){
  freshState(from,step,70,false); wantFreq=to; int n=0;
  // reference: the dial as a fresh full redraw at the target
  std::vector<uint16_t> ref; { wantFreq=to; drawRadioAnalog(true); ref=region(4,18,232,30); wantFreq=from; drawRadioAnalog(true); wantFreq=to; }
  for(n=1;n<=200;n++){ drawRadioAnalog(false); if(region(4,18,232,30)==ref) return n; }
  return -1; }
// ---- screen changes (the real loop() dispatch), globe screen with and without the Mac, and the Mac notice
static std::vector<uint16_t> shot(){ return lcdObj.px; }
static void blank(){ lcdObj.fillScreen(0); }
static void loopOnce(){ KeyInput k; KnobInput kn; loopDispatch(k,kn); }
static void stateConnected(){ rem.ok=true; strcpy(rem.link,"connected"); rem.serverIndex=0; wifiStatusVal=3; strcpy(netStatus,"PC: OK");
  const char* nm[4]={"Osaka JP","Rotorua NZ","Ironstone AU","Ironstone AU2"}; float la[4]={34.8585f,-38.1521f,-34.9644f,-34.9644f}, lo[4]={135.5144f,176.0708f,138.7624f,138.7624f};
  srvCount=4; for(int i=0;i<4;i++){ strcpy(srvs[i].shortName,nm[i]); strcpy(srvs[i].name,nm[i]); srvs[i].lat=la[i]; srvs[i].lon=lo[i]; srvs[i].hasPos=true; } }
static void stateMacDown(){ rem.ok=false; strcpy(rem.link,""); wifiStatusVal=3; strcpy(netStatus,"PC: connection refused"); srvCount=0; }
static void savePpm(const char* name){ char p[96]; snprintf(p,96,"%s/%s.ppm",getenv("OUT"),name); ppm(p); }

// radio screen first, then the G key: is the globe screen identical to a globe drawn on a clean screen?
static void maskSat(std::vector<uint16_t>& v);
static void globeAfterRadio(const char* label, bool macUp, const char* pic){
  if(macUp) stateConnected(); else stateMacDown();
  // reference: the globe on a clean screen
  blank(); screen=SC_RADIO; screenDirty=true; fakeMs+=5000; globeOpen(); loopOnce(); auto ref=shot();
  globeClose(); loopOnce();
  // the real sequence: the radio screen is showing, then the key
  blank(); screen=SC_RADIO; screenDirty=true; fakeMs+=5000; loopOnce(); loopOnce();   // the radio (or the Mac notice) is on screen
  gKeyG=true; loopOnce();                                                             // the key press: switches to the globe
  loopOnce();                                                                         // the next pass draws the globe
  auto got=shot(); if(pic) savePpm(pic); maskSat(got); maskSat(ref);
  size_t diff=0; for(size_t i=0;i<ref.size();i++) if(ref[i]!=got[i]) diff++;
  char m[160]; snprintf(m,160,"%s: globe screen after the radio screen is identical to a clean globe (%zu pixels differ)",label,diff); check(diff==0,m);
  // and back with Esc: the radio screen is complete again, nothing of the globe is left
  gKeyEsc=true; loopOnce(); loopOnce(); auto back=shot();
  blank(); screen=SC_RADIO; screenDirty=true; fakeMs+=5000; loopOnce(); auto radioRef=shot();
  size_t d2=0; for(size_t i=0;i<back.size();i++) if(back[i]!=radioRef[i]) d2++;
  snprintf(m,160,"%s: after Esc the radio screen is complete (%zu pixels differ from a clean one)",label,d2); check(d2==0,m);
}


// ---- v1.10 metal receiver: the checks run on the firmware's own drawing code (printLog = every text it printed)
static void baseState(){
  stateConnected(); strcpy(rem.link,"connected"); rem.ok=true; rem.s=7; rem.sOver=0; strcpy(rem.server,"Ironstone Range"); strcpy(rem.error,""); strcpy(netStatus,"PC: OK");
  wantOutput=0; auWanted=false; auConn=false; auNoMem=false; auPlayout.playing=false; auNetState=2; tuneAuto=false; strcpy(rem.scan,"off"); strcpy(rem.scanMsg,""); rem.scanDoneMs=0; autoHintUntil=0;
  wifiState=W_UP; wifiStatusVal=3; savedCount=1; wantFreq=7100; stepIdx=1; wantMode=0; wantVolume=70; wantMuted=false; }
static void freshDraw(){ printLog.clear(); recOn=true; drawRadioAnalog(true); recOn=false; }
static std::string lcdLine(int n){ for(auto&r:printLog) if(r.onLcd && r.x>=GLASS_X && r.x<GLASS_X+LCD_MAIN_W && r.y==(n==0?GLASS_Y+6:GLASS_Y+22)) return r.s; return "<none>"; }
static bool printed(const std::string& s, bool onLcd=true){ for(auto&r:printLog) if(r.s==s && r.onLcd==onLcd) return true; return false; }
static uint16_t pixel(int x,int y){ return lcdObj.px[y*240+x]; }
static std::vector<uint16_t> meterRegion(bool vol){ return region(vol?VOL_X:SIG_X,METER_Y,METER_W,METER_H); }
// the needle angle (degrees from vertical, left negative) read back from the pixels: the needle is the only DL_RED in the face
static float needleAngle(bool vol){
  float best=-1,ang=0; for(int y=0;y<METER_H;y++)for(int x=0;x<METER_W;x++){ if(pixel((vol?VOL_X:SIG_X)+x,METER_Y+y)!=DL_RED) continue; float dx=x-MT_PX, dy=MT_PY-y; float d=sqrtf(dx*dx+dy*dy); if(d>best){ best=d; ang=atan2f(dx,dy)*180.0f/M_PI; } }
  return best<0?-999.0f:ang; }
static float A2(float deg){ float r=deg*M_PI/180.0f; return atan2f(MT_K*sinf(r),cosf(r))*180.0f/M_PI; }  // the arc is stretched sideways
static void testsNew(){
  char m[200];
  // --- geometry: everything we draw sits inside 240 x 135
  check(DIAL_X+DIAL_W<=236 && DIAL_Y+DIAL_H<=90 && VOL_X+METER_W<=107 && METER_Y+METER_H<=55 && IND_X+IND_W<=236 && IND_Y+IND_H<=55 && GLASS_X+GLASS_W<=162 && CLOCK_X+CLOCK_W<=236 && GLASS_Y+GLASS_H<=130,"layout constants: upper instruments, scale and separate displays stay on the screen without overlap");
  // --- fixed texts, never abbreviated
  baseState(); freshDraw();
  check(printed("RADIO DAYS SDR"),"the full name RADIO DAYS SDR is printed (SDR not dropped)");
  check(printed("OUT PC") && printed("OUT ADV"),"both fixed labels OUT PC and OUT ADV are printed");
  check(printed("WiFi")&&printed("PC")&&printed("Kiwi"),"the link row uses full WiFi / PC / Kiwi labels");
  check(printed("AM")&&printed("USB")&&printed("LSB")&&printed("MANUAL")&&printed("AUTO"),"mode and MANUAL/AUTO labels are in the indicator panel");
  check(printed(".1")&&printed("1")&&printed("5")&&printed("10")&&printed("kHz"),"all four tuning increments and their unit are visible");
  check(defaultStepForMode(0)==2 && defaultStepForMode(1)==1 && defaultStepForMode(2)==1,"mode defaults are AM 5 kHz and USB/LSB 1 kHz");
  { bool ok=true; for(auto&r:printLog) if(r.onLcd){ int tw=(int)r.s.size()*6; if(r.x<0||r.x+tw>240) ok=false; } check(ok,"no printed text leaves the screen (baseline state)"); }
  // --- LCD: exact frequency and a separate two-line JST clock module.
  { float fs[]={10.0f,86.4f,795.0f,9580.3f,15770.0f,23456.7f,30000.0f}; bool ok=true; std::set<uint64_t> sig;
    for(float f: fs){ baseState(); tuneAuto=true; wantMode=1; stepIdx=3; wantFreq=f; freshDraw(); uint64_t h=1469598103934665603ULL;
      for(int y=GLASS_Y+1;y<GLASS_Y+18;y++)for(int x=GLASS_X+1;x<GLASS_X+LCD_MAIN_W-1;x++){h^=lcd.px[y*240+x];h*=1099511628211ULL;}
      sig.insert(h); if(!printed("kHz")) ok=false; }
    check(ok && sig.size()==7,"LCD main line 1 renders seven distinct exact bold frequencies with one decimal and kHz"); }
  { baseState(); freshDraw(); int lit=0; for(int y=106;y<122;y++)for(int x=174;x<228;x++) if(lcd.px[y*240+x]==GL_BRIGHT) lit++;
    check(printed("SAT")&&printed("10/25")&&lit>70,"the digital-watch display shows weekday/date and a large bold JST time"); }
  // --- LCD line 2: the states
  struct L2{ const char* what; void(*set)(); const char* want; };
  static const L2 L2S[]={
    {"connected",[]{},"Kiwi OK: Ironstone Range"},
    {"Kiwi connecting",[]{strcpy(rem.link,"connecting");},"Kiwi: connecting Ironstone Range"},
    {"Kiwi error",[]{strcpy(rem.link,"error"); strcpy(rem.error,"TimeoutError: timed out");},"Kiwi: TimeoutError: timed out"},
    {"Wi-Fi down",[]{wifiStatusVal=0; wifiState=W_UP; rem.ok=false;},"Wi-Fi: no connection (retry)"},
    {"Wi-Fi not set",[]{wifiStatusVal=0; savedCount=0; rem.ok=false;},"Wi-Fi: not set. Press W"},
    {"PC not answering",[]{rem.ok=false; strcpy(netStatus,"PC: connection refused");},"PC: connection refused"},
    {"ADV reconnecting",[]{wantOutput=1; auWanted=true; auConn=false; auNetState=4;},"ADV audio: reconnecting"},
    {"ADV buffering",[]{wantOutput=1; auWanted=true; auConn=true; auPlayout.playing=false;},"ADV audio: buffering"},
    {"ADV playing",[]{wantOutput=1; auWanted=true; auConn=true; auPlayout.playing=true;},"Kiwi OK: Ironstone Range"},
    {"ADV no memory",[]{wantOutput=1; auNoMem=true;},"ADV audio: no memory - Mac speaker"},
    {"scanning up",[]{tuneAuto=true; strcpy(rem.scan,"up"); strcpy(rem.scanMsg,"scanning");},"SCANNING >>>  Esc / push: stop"},
    {"scanning down",[]{tuneAuto=true; strcpy(rem.scan,"down"); strcpy(rem.scanMsg,"scanning");},"SCANNING <<<  Esc / push: stop"},
    {"signal found",[]{tuneAuto=true; strcpy(rem.scanMsg,"signal"); rem.scanExcess=22; rem.scanDoneMs=fakeMs;},"SIGNAL FOUND  +22 dB"},
    {"end of band",[]{tuneAuto=true; strcpy(rem.scanMsg,"end"); rem.scanDoneMs=fakeMs;},"END OF BAND"},
    {"scan: Kiwi lost",[]{tuneAuto=true; strcpy(rem.scanMsg,"lost"); rem.scanDoneMs=fakeMs;},"SCAN ENDED: KIWI LINK LOST"},
    {"scan stopped",[]{tuneAuto=true; strcpy(rem.scanMsg,"stopped"); rem.scanDoneMs=fakeMs;},"SCAN STOPPED"},
    {"AUTO hint",[]{tuneAuto=true; autoHintUntil=fakeMs+100000;},"AUTO: turn the knob to search"},
  };
  { bool ok=true; std::string firstBad; bool fit=true;
    for(const L2& c: L2S){ baseState(); c.set(); freshDraw(); std::string got=lcdLine(1), want=std::string(c.want).substr(0,24); if(got!=want){ ok=false; if(firstBad.empty()) firstBad=std::string(c.what)+": '"+got+"' != '"+want+"'"; } if((int)got.size()>24) fit=false; }
    snprintf(m,sizeof m,"LCD line 2 shows all %zu states%s%s",sizeof(L2S)/sizeof(L2S[0]),ok?"":": ",ok?"":firstBad.c_str()); check(ok,m);
    check(fit,"LCD line 2 never exceeds 24 characters beside the clock"); }
  { baseState(); strcpy(rem.link,"error"); strcpy(rem.error,"0123456789012345678901234567890123456789"); freshDraw(); std::string g=lcdLine(1); check((int)g.size()<=24 && g.size()>20,"a very long error text is cut safely beside the clock"); }
  // --- needles: the angle read back from the pixels
  { bool ok=true; std::string bad; float vols[]={0,25,50,75,100};
    for(float v: vols){ baseState(); wantVolume=(int)v; freshDraw(); float a=needleAngle(true), want=A2(-MT_SPAN+2*MT_SPAN*v/100); if(fabsf(a-want)>3.5f){ ok=false; bad+="vol "+std::to_string((int)v)+" angle "+std::to_string(a)+"; "; } }
    snprintf(m,sizeof m,"VOL needle follows the volume setting 0/25/50/75/100%s%s",ok?"":": ",bad.c_str()); check(ok,m); }
  { bool ok=true; std::string bad; struct S{ float s; int over; }; S ss[]={{0,0},{1,0},{5,0},{9,0},{9,20},{9,40}};
    for(auto& c: ss){ baseState(); rem.s=c.s; rem.sOver=c.over; freshDraw(); float frac=(c.s+(c.over?c.over/20.0f:0))/11.0f; float want=A2(-MT_SPAN+2*MT_SPAN*std::min(frac,1.0f)), a=needleAngle(false); if(fabsf(a-want)>3.5f){ ok=false; bad+="s "+std::to_string(c.s)+"+"+std::to_string(c.over)+" angle "+std::to_string(a)+" want "+std::to_string(want)+"; "; } }
    snprintf(m,sizeof m,"SIGNAL needle follows S0..S9 and over S9%s%s",ok?"":": ",bad.c_str()); check(ok,m); }
  { baseState(); strcpy(rem.link,"connecting"); rem.s=9; freshDraw(); float a=needleAngle(false); check(fabsf(a-A2(-MT_SPAN))<3.5f,"SIGNAL needle rests at zero while the receiver is not connected"); }
  { baseState(); wantMuted=true; freshDraw(); float a=needleAngle(true); check(fabsf(a-A2(-MT_SPAN))<3.5f && printed("MUTE",false) && !printed("VOLUME",false),"MUTE: the VOL needle goes to zero and the face says MUTE instead of VOLUME"); }
  { baseState(); freshDraw(); check(printed("VOLUME",false) && printed("SIGNAL",false),"the faces are labelled SIGNAL and VOLUME"); }
  { baseState(); wantVolume=40; freshDraw(); auto a=meterRegion(true); wantVolume=45; drawRadioAnalog(false); int n=0; baseState(); wantVolume=45; freshDraw(); auto b=meterRegion(true);
    wantVolume=40; freshDraw(); wantVolume=70; for(n=1;n<=100;n++){ drawRadioAnalog(false); } baseState(); freshDraw(); auto c=meterRegion(true);
    check(a!=b,"one volume step (5) moves the VOLUME needle"); check(meterRegion(true)==c,"the VOL needle settles exactly on the set value after easing"); }
  { baseState(); rem.s=2; freshDraw(); rem.s=9; for(int i=0;i<100;i++) drawRadioAnalog(false); auto settled=meterRegion(false); baseState(); rem.s=9; freshDraw(); check(settled==meterRegion(false),"the SIGNAL needle settles exactly on the new S value after easing"); }
  // --- lamps
  { const uint16_t G=rgb565c(70,205,60), A=rgb565c(238,196,34), R=rgb565c(214,44,36), D=rgb565c(52,55,53);
    baseState(); freshDraw(); check(pixel(119,9)==A && pixel(176,9)==D,"OUT PC: selected is amber, OUT ADV is dark");
    baseState(); wantOutput=1; auWanted=true; auConn=true; auPlayout.playing=true; freshDraw(); check(pixel(119,9)==D && pixel(176,9)==A,"OUT ADV: selected is amber, OUT PC is dark");
    baseState(); wantOutput=1; auNoMem=true; freshDraw(); check(pixel(176,9)==R,"OUT ADV no memory: its lamp is red");
    baseState(); freshDraw(); check(pixel(119,18)==G && pixel(157,18)==G && pixel(195,18)==G,"WiFi / PC / Kiwi all green when connected");
    baseState(); wifiStatusVal=0; wifiState=W_TRY; rem.ok=false; freshDraw(); check(pixel(119,18)==A,"WiFi is amber while connecting");
    baseState(); wifiStatusVal=0; wifiState=W_UP; rem.ok=false; freshDraw(); check(pixel(119,18)==R && pixel(157,18)==D && pixel(195,18)==D,"WiFi down: red, PC and Kiwi dark");
    baseState(); rem.ok=false; freshDraw(); check(pixel(119,18)==G && pixel(157,18)==A && pixel(195,18)==D,"PC not answering: WiFi green, PC amber, Kiwi dark");
    baseState(); strcpy(rem.link,"connecting"); freshDraw(); check(pixel(157,18)==G && pixel(195,18)==A,"Kiwi connecting: Kiwi amber");
    baseState(); strcpy(rem.link,"error"); freshDraw(); check(pixel(195,18)==R,"Kiwi error: Kiwi red");
    baseState(); freshDraw(); check(pixel(119,27)==A&&pixel(176,27)==D,"MANUAL selection is amber on row 3");
    baseState(); tuneAuto=true; freshDraw(); check(pixel(119,27)==D&&pixel(176,27)==A,"AUTO selection is amber on row 3");
    baseState(); wantMode=0; freshDraw(); check(pixel(119,36)==A&&pixel(157,36)==D&&pixel(195,36)==D,"AM selection is amber on row 4");
    baseState(); wantMode=1; freshDraw(); check(pixel(119,36)==D&&pixel(157,36)==A&&pixel(195,36)==D,"USB selection is amber on row 4");
    baseState(); wantMode=2; freshDraw(); check(pixel(119,36)==D&&pixel(157,36)==D&&pixel(195,36)==A,"LSB selection is amber on row 4");
    { bool ok=true; const int xs[4]={119,147,169,191}; for(int si=0;si<4;si++){ baseState(); stepIdx=si; freshDraw(); for(int j=0;j<4;j++) if(pixel(xs[j],45)!=(j==si?A:D))ok=false; } check(ok,"one of .1 / 1 / 5 / 10 kHz is amber for every step"); }
    // changing a lamp in a running screen paints the same pixels as a fresh draw
    baseState(); freshDraw(); strcpy(rem.link,"error"); drawRadioAnalog(false); auto live=region(IND_X,IND_Y,IND_W,IND_H); baseState(); strcpy(rem.link,"error"); freshDraw(); check(live==region(IND_X,IND_Y,IND_W,IND_H),"a lamp change drawn live equals a fresh draw"); }
  // --- the scale: labels and movement
  { baseState(); wantFreq=7100; freshDraw(); bool l=false; for(auto&r:printLog) if(!r.onLcd&&r.s=="7100"&&r.x==DIAL_W/2) l=true; check(l,"step 1: the label 7100 sits under the red line at 7100.0");
    baseState(); wantFreq=7100; stepIdx=0; freshDraw(); l=false; for(auto&r:printLog) if(!r.onLcd&&r.s=="7100"&&r.x==DIAL_W/2) l=true; check(l,"step 0.1: the label 7100 sits under the red line at 7100.0");
    baseState(); wantFreq=7100; stepIdx=2; freshDraw(); l=false; for(auto&r:printLog) if(!r.onLcd&&r.s=="7100"&&r.x==DIAL_W/2) l=true; check(l,"step 5: the label 7100 sits under the red line at 7100.0");
    baseState(); wantFreq=7100; freshDraw(); auto a=region(DIAL_X,DIAL_Y,DIAL_W,DIAL_H); baseState(); wantFreq=7100; freshDraw(); wantFreq=7101; drawRadioAnalog(false); auto b=region(DIAL_X,DIAL_Y,DIAL_W,DIAL_H);
    check(a!=b,"the scale moves when the frequency changes"); baseState(); wantFreq=7100; stepIdx=2; freshDraw(); auto c=region(DIAL_X,DIAL_Y,DIAL_W,DIAL_H); check(a!=c,"the scale looks different for a different step");
    bool red=true; for(int y=0;y<DIAL_H;y++) if(pixel(DIAL_X+DIAL_W/2-1,DIAL_Y+y)!=DL_RED) red=false; check(red,"the thin red line is fixed at the centre of the scale and spans its full height"); }
  { int steps[4]={0,1,2,3}; for(int si: steps){
      auto settle=[&](float from,float to){ baseState(); stepIdx=si; wantFreq=from; freshDraw(); wantFreq=to; printLog.clear();
        baseState(); stepIdx=si; wantFreq=to; freshDraw(); auto ref=region(DIAL_X,DIAL_Y,DIAL_W,DIAL_H); baseState(); stepIdx=si; wantFreq=from; freshDraw(); wantFreq=to; int n; for(n=1;n<=200;n++){ drawRadioAnalog(false); if(region(DIAL_X,DIAL_Y,DIAL_W,DIAL_H)==ref) return n; } return -1; };
      int n1=settle(10000,10000+STEPS[si]), n2=settle(10000,10000+STEPS[si]*38), n3=settle(9000,20000);
      snprintf(m,sizeof m,"step %.1f: the scale settles exactly (1 step: %d frames, 38 steps: %d, 11000 kHz jump: %d)",STEPS[si],n1,n2,n3); check(n1>0&&n2>0&&n3>0&&n1<=6&&n2<=30,m); } }
  // --- idle frames draw nothing; fonts do not leak
  baseState(); freshDraw(); { auto a=region(0,0,240,135); for(int i=0;i<5;i++) drawRadioAnalog(false); check(a==region(0,0,240,135),"idle frames leave the screen unchanged"); }
  check(lcdObj.font==nullptr,"after the radio screen the built-in font is active again");
  lcdObj.font=&FreeSansBold9pt7b; txt(0,0,1,0xFFFF,"AB"); check(lcdObj.font==nullptr,"txt() heals a stuck GFX font (built-in font always used for txt)");
  { lcdObj.setFont(fonts::FreeSansBold9pt7b); lcdObj.setTextFont(1); check(lcdObj.font!=nullptr,"model check: setTextFont(1) is a no-op while a GFX font is active (as in M5GFX)"); lcdObj.setFont(fonts::Font0); }
  // --- memory: the two sprites together are smaller than before (v1.8: 232x30 + 73x56 px at 16 bit)
  { int now=(DIAL_W*DIAL_H+METER_W*METER_H)*2, before=(232*30+73*56)*2; snprintf(m,sizeof m,"sprite memory %d bytes (v1.8: %d)",now,before); check(now<=before,m); }
  // --- the old elements are gone from the radio screen
  { baseState(); freshDraw(); check(!printed("STEP")&&!printed("L"),"no separate STEP or level-bar label remains outside the display"); }
}

static void testKnobLogic(){
  wantMode=0; stepIdx=0; verMode=0; radioDirty=false; changeMode(1);
  check(wantMode==1 && stepIdx==1 && verMode==1 && radioDirty,"USB mode change selects its 1 kHz default");
  stepIdx=3; changeMode(1);
  check(stepIdx==3 && verMode==1,"selecting the current mode again preserves a manual step choice");
  changeMode(0);
  check(wantMode==0 && stepIdx==2 && verMode==2,"AM mode change selects its 5 kHz default");
  changeMode(2);
  check(wantMode==2 && stepIdx==1 && verMode==3,"LSB mode change selects its 1 kHz default");
  tuneAuto=false; wantScanDir=0; verScan=0; strcpy(rem.scan,"off");
  setTuneAuto(true); check(tuneAuto && verScan==0,"switching to AUTO does not start a scan by itself");
  autoTurn(+1); check(wantScanDir==1 && verScan==1,"AUTO: turning right asks the Mac to scan upward");
  strcpy(rem.scan,"up"); autoTurn(+3); check(verScan==1,"AUTO: more clicks in the same direction while scanning change nothing");
  autoTurn(-1); check(wantScanDir==-1 && verScan==2,"AUTO: turning the other way reverses the scan");
  strcpy(rem.scan,"off"); autoTurn(-1); check(verScan==3,"AUTO: after it stopped on a signal, the next turn searches again in that direction");
  strcpy(rem.scan,"up"); setTuneAuto(false); check(!tuneAuto && wantScanDir==0 && verScan==4,"back to MANUAL stops the scan");
  strcpy(rem.scan,"off");
  // the SIGNAL FOUND message goes away after a few seconds
  baseState(); tuneAuto=true; strcpy(rem.scanMsg,"signal"); rem.scanExcess=22; rem.scanDoneMs=fakeMs; freshDraw(); std::string a=lcdLine(1);
  fakeMs+=6000; baseState(); tuneAuto=true; strcpy(rem.scanMsg,"signal"); rem.scanDoneMs=fakeMs-6000; freshDraw(); std::string b=lcdLine(1);
  check(a=="SIGNAL FOUND  +22 dB" && b=="Kiwi OK: Ironstone Range","the SIGNAL FOUND message goes away after a few seconds");
  // while scanning the scale glides slower than a manual step and still ends exactly on the frequency
  baseState(); tuneAuto=true; strcpy(rem.scan,"up"); wantFreq=9500; stepIdx=2; drawRadioAnalog(true); wantFreq=9505; for(int i=0;i<6;i++) drawRadioAnalog(false); auto glide=region(DIAL_X,DIAL_Y,DIAL_W,DIAL_H);
  baseState(); tuneAuto=true; strcpy(rem.scan,"up"); wantFreq=9505; stepIdx=2; blank(); drawRadioAnalog(true); auto exact=region(DIAL_X,DIAL_Y,DIAL_W,DIAL_H);
  check(glide!=exact,"while scanning the scale is still gliding after 6 frames (it follows the scan)");
  baseState(); tuneAuto=true; strcpy(rem.scan,"up"); wantFreq=9500; stepIdx=2; drawRadioAnalog(true); wantFreq=9505; int n=0; for(n=1;n<=150;n++){ drawRadioAnalog(false); if(region(DIAL_X,DIAL_Y,DIAL_W,DIAL_H)==exact) break; }
  char m[100]; snprintf(m,100,"... and it ends exactly on the new frequency (%d frames = %.1f s)",n,n*0.033); check(n<=100,m);
  baseState();
}

// the satellite: floats, talks (cyan, blink, yellow), never draws over the disc / header / panel, leaves no residue
static void maskSat(std::vector<uint16_t>& v){ for(int y=14;y<60;y++)for(int x=56;x<100;x++) v[y*240+x]=0; }
static void testSatellite(){
  char m[200];
  baseState(); blank(); screen=SC_RADIO; screenDirty=true; fakeMs+=5000; globeOpen(); for(int i=0;i<30;i++) loopOnce();
  const int ecx=GLOBE_X+GLOBE_R, ecy=GLOBE_Y+GLOBE_R;
  std::vector<std::pair<int,int>> poses; bool seenDown=false, seenBlink=false, seenUp=false, order=true, cyan=false, yellow=false, panelClear=true, residueOk=true, bounds=true; int stage=0; int checked=0; std::string firstBad;
  for(int i=0;i<9000 && !(seenUp && checked>40);i++){
    loopOnce(); const SatPose& p=satDrawn; if(!satShown) continue;
    if(std::find(poses.begin(),poses.end(),std::make_pair(p.x,p.y))==poses.end()) poses.push_back({p.x,p.y});
    if(p.y<14 || p.x<56 || p.x+SAT_W>110) bounds=false;
    auto sc=shot();
    for(int y=16;y<21;y++)for(int x=100;x<140;x++) if(sc[y*240+x]) panelClear=false;
    // waves: colours, and the order down -> blink -> up
    int st = p.wave==1?1 : (p.lampMode?2 : (p.wave==2?3:0));
    if(st==1){ seenDown=true; if(stage>1) order=false; stage=std::max(stage,1); }
    if(st==2){ seenBlink=true; if(stage<1) order=false; stage=std::max(stage,2); }
    if(st==3){ seenUp=true; if(stage<2) order=false; stage=3; }
    if(st==1||st==3) for(int y=16;y<60;y++)for(int x=40;x<100;x++){ uint16_t c=sc[y*240+x]; int r=(c>>11)<<3, g=((c>>5)&63)<<2, b=(c&31)<<3; if(st==1 && r<130 && g>130 && b>170 && g>=b-40) cyan=true; if(st==3 && r>150 && g>120 && b<80) yellow=true; }
    // no residue: outside the disc the region is black plus exactly the current pose
    if(i%3==0){ auto saved=lcdObj.px; for(int y=16;y<60;y++)for(int x=40;x<100;x++){ int dx=x-ecx, dy=y-ecy; if(dx*dx+dy*dy>(GLOBE_R+1)*(GLOBE_R+1)) lcdObj.px[y*240+x]=0; } satPaint(p,false); auto ex=lcdObj.px; lcdObj.px=saved; checked++;
      for(int y=16;y<60&&residueOk;y++)for(int x=40;x<100;x++){ if(sc[y*240+x]!=ex[y*240+x]){ residueOk=false; snprintf(m,sizeof m,"first difference at %d,%d (pose %d,%d wave %d step %d)",x,y,p.x,p.y,p.wave,p.waveStep); firstBad=m; break; } } }
  }
  snprintf(m,sizeof m,"the satellite floats: %zu different positions seen",poses.size()); check(poses.size()>=4,m);
  check(bounds,"the satellite stays clear of the header and inside the left of the panel");
  check(seenDown&&seenBlink&&seenUp&&order,"it talks in the order of U4: Earth to satellite, lamp blink, satellite to Earth");
  check(cyan,"Earth to satellite: cyan wave fronts are drawn"); check(yellow,"satellite to Earth: yellow wave fronts are drawn");
  check(panelClear,"nothing is drawn into the panel's top rows"); snprintf(m,sizeof m,"no residue: outside the disc the screen is exactly black plus the current pose (%d checks)%s%s",checked,residueOk?"":": ",firstBad.c_str()); check(residueOk&&checked>40,m);
  // the globe turns: the pose is drawn again over the pushed sprite
  gSel=(gSel+1)%std::max(1,srvCount); globeAim(false); for(int i=0;i<60;i++) loopOnce();
  { auto sc=shot(); auto saved=lcdObj.px; for(int y=16;y<60;y++)for(int x=40;x<100;x++){ int dx=x-ecx, dy=y-ecy; if(dx*dx+dy*dy>(GLOBE_R+1)*(GLOBE_R+1)) lcdObj.px[y*240+x]=0; } satPaint(satDrawn,false); auto ex=lcdObj.px; lcdObj.px=saved; bool same=true; for(int y=16;y<60;y++)for(int x=40;x<100;x++) if(sc[y*240+x]!=ex[y*240+x]) same=false; check(same,"after the globe has turned the satellite is still complete"); }
  // pictures: the four moments of U4's preview
  { char pth[96]; struct Want{ const char* n; int st; } W[4]={{"w0_sat_float",0},{"w1_sat_earth_to_sat",1},{"w2_sat_blink",2},{"w3_sat_to_earth",3}}; bool got[4]={false,false,false,false};
    for(int i=0;i<9000;i++){ loopOnce(); if(!satShown) continue; const SatPose& p=satDrawn; int st=p.wave==1?1:(p.lampMode?2:(p.wave==2?3:0)); if(st==0 && p.wave==0 && !p.lampMode && !satTalking) st=0; else if(st==0) continue;
      if(st==1 && p.waveStep<12) continue; if(st==3 && p.waveStep<12) continue; if(got[st]) continue; got[st]=true;
      snprintf(pth,96,"%s/%s.ppm",getenv("OUT"),W[st].n); ppm(pth); snprintf(pth,96,"%s/%s_1x.ppm",getenv("OUT"),W[st].n); ppm1(pth); if(got[0]&&got[1]&&got[2]&&got[3]) break; } }
  globeClose(); loopOnce();
}

static void tests2(){
  globeAfterRadio("Mac connected", true, "o_globe_after_radio_mac_connected");
  globeAfterRadio("Mac NOT connected", false, "p_globe_after_radio_mac_down");
  // the diagnostics screen the same way
  stateConnected(); blank(); screen=SC_DIAG; screenDirty=true; loopOnce(); auto dref=shot();
  gKeyEsc=false; screen=SC_RADIO; screenDirty=true; loopOnce(); gKeyD=true; loopOnce(); loopOnce(); auto dgot=shot();
  size_t dd=0; for(size_t i=0;i<dref.size();i++) if(dref[i]!=dgot[i]) dd++; { char m[120]; snprintf(m,120,"diagnostics screen after the radio screen is clean (%zu pixels differ)",dd); check(dd==0,m); }

  // ---- the Mac notice
  stateConnected(); blank(); screen=SC_RADIO; screenDirty=true; fakeMs+=5000; loopOnce(); auto normal=shot();
  stateMacDown(); blank(); drawMacNotice(true); auto noticeRef=shot(); savePpm("n_mac_notice");
  stateMacDown(); blank(); screen=SC_RADIO; screenDirty=true; fakeMs+=100000; loopOnce();     // first moment without the Mac
  { size_t d=0; auto s1=shot(); for(size_t i=0;i<s1.size();i++) if(s1[i]!=noticeRef[i]) d++; check(d>0,"no notice in the first seconds without the Mac (no flicker on a short hiccup)"); }
  fakeMs+=4000; loopOnce(); { auto s2=shot(); size_t d=0; for(size_t i=0;i<s2.size();i++) if(s2[i]!=noticeRef[i]) d++; char m[100]; snprintf(m,100,"after 3+ s without the Mac the notice is shown (%zu pixels differ)",d); check(d==0,m); }
  stateConnected(); loopOnce(); loopOnce(); { auto s3=shot(); blank(); screen=SC_RADIO; screenDirty=true; loopOnce(); auto fresh=shot(); size_t d=0; for(size_t i=0;i<fresh.size();i++) if(s3[i]!=fresh[i]) d++; char m[110]; snprintf(m,110,"once the Mac answers the normal screen is back and complete (%zu pixels differ)",d); check(d==0,m); }
  // not shown when the cause is elsewhere
  stateMacDown(); wifiStatusVal=0; blank(); screen=SC_RADIO; screenDirty=true; loopOnce(); fakeMs+=10000; loopOnce(); { auto s4=shot(); size_t d=0; for(size_t i=0;i<s4.size();i++) if(s4[i]!=noticeRef[i]) d++; check(d>0,"Wi-Fi down: no Mac notice (the Wi-Fi status is shown instead)"); }
  stateConnected(); strcpy(rem.link,"error"); strcpy(rem.error,"TimeoutError: timed out"); blank(); screen=SC_RADIO; screenDirty=true; fakeMs+=10000; loopOnce(); loopOnce(); { auto s5=shot(); size_t d=0; for(size_t i=0;i<s5.size();i++) if(s5[i]!=noticeRef[i]) d++; check(d>0,"KiwiSDR failure with the Mac answering: no Mac notice"); }
  strcpy(rem.link,"connected"); strcpy(rem.error,"");
  // the text fits: nothing touches the right edge, other error texts too
  const char* errs[]={"PC: connection refused","PC: read Timeout","PC: HTTP 403","PC: bad address","PC: searching...","PC: send header failed"};
  for(const char* e: errs){ stateMacDown(); strcpy(netStatus,e); blank(); drawMacNotice(true); bool edge=false; for(int y=16;y<135;y++) for(int x=236;x<240;x++) if(lcdObj.px[y*240+x]) edge=true; char m[100]; snprintf(m,100,"notice text fits the screen with '%s'",e); check(!edge,m); }
  stateConnected(); blank(); screen=SC_RADIO; screenDirty=true; globeOpen(); loopOnce(); { bool edge=false; for(int y=120;y<135;y++) for(int x=236;x<240;x++) if(lcdObj.px[y*240+x]) edge=true; check(!edge,"globe screen: the hint line at the bottom fits (not cut off at the right edge)"); }
  globeClose(); loopOnce();
  stateConnected();
}

int main(){  dialSpriteObj.createSprite(DIAL_W,DIAL_H); meterSpriteObj.createSprite(METER_W,METER_H);
  baseState(); frame("s01_normal_outpc_manual");
  baseState(); wantOutput=1; auWanted=true; auConn=true; auPlayout.playing=true; tuneAuto=true; wantMode=1; stepIdx=2; wantFreq=15770; rem.s=5; wantVolume=40; frame("s02_adv_auto_usb");
  baseState(); wantMuted=true; frame("s03_mute");
  baseState(); strcpy(rem.link,"connecting"); frame("s04_kiwi_connecting");
  baseState(); strcpy(rem.link,"error"); strcpy(rem.error,"TimeoutError: timed out"); frame("s05_kiwi_error");
  baseState(); tuneAuto=true; strcpy(rem.scan,"up"); strcpy(rem.scanMsg,"scanning"); wantFreq=9500; stepIdx=2; frame("s06_auto_scanning");
  baseState(); tuneAuto=true; strcpy(rem.scanMsg,"signal"); rem.scanExcess=22; rem.scanDoneMs=fakeMs; wantFreq=9580.3f; stepIdx=0; rem.s=9; rem.sOver=20; frame("s07_signal_found");
  baseState(); wifiStatusVal=0; wifiState=W_UP; rem.ok=false; frame("s08_wifi_down");
  baseState(); rem.ok=false; strcpy(netStatus,"PC: connection refused"); frame("s09_pc_not_answering");
  baseState(); wantOutput=1; auWanted=true; auConn=true; auPlayout.playing=false; frame("s10_adv_buffering");
  baseState(); wantOutput=1; auNoMem=true; frame("s11_adv_no_memory");
  baseState(); wantMode=2; stepIdx=3; wantFreq=30000; wantVolume=100; rem.s=3; frame("s12_lsb_step10_30000");
  baseState(); stepIdx=0; wantFreq=10.0f; wantVolume=0; frame("s13_step0.1_10kHz_vol0");
  baseState(); strcpy(rem.link,"error"); strcpy(rem.error,"0123456789012345678901234567890123456789"); frame("s14_long_error");
  baseState(); wantOutput=1; auWanted=true; auConn=true; auPlayout.playing=true;
  lcdObj.setFont(fonts::FreeSansBold9pt7b);  // even with a GFX font left active, the diagnostics screen must use the built-in font
  drawDiag(true); { char p[96]; snprintf(p,96,"%s/l_diag.ppm",getenv("OUT")); ppm(p); }
  lcdObj.setFont(fonts::Font0);
  baseState(); testsNew(); testKnobLogic(); tests2(); testSatellite(); printf("TOTAL %d failure(s)\n", fails); printf("harness ok\n"); return fails?1:0; }
