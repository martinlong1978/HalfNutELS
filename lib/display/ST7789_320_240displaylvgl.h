
#include <config.h>
#include <globalstate.h>
#include <leadscrew.h>
#include <spindle.h>


#include <lvgl.h>
#include <TFT_eSPI.h>
#include <SPI.h>

LV_IMAGE_DECLARE(feedSymbol);
LV_IMAGE_DECLARE(threadSymbol);
LV_IMAGE_DECLARE(leftstop);
LV_IMAGE_DECLARE(rightstop);
LV_IMAGE_DECLARE(unlocked);
LV_IMAGE_DECLARE(locked);
LV_IMAGE_DECLARE(left);
LV_IMAGE_DECLARE(right);
LV_IMAGE_DECLARE(pauseSymbol);
LV_IMAGE_DECLARE(syncSymbol);
LV_IMAGE_DECLARE(jog);

#define DRAW_BUF_SIZE ((TFT_WIDTH * TFT_HEIGHT / 10) * (LV_COLOR_DEPTH / 8))


class Display {
private:
  bool initOta = false;
  Spindle* m_spindle;
  Leadscrew* m_leadscrew;
  GlobalState* m_globalState;
#ifdef ELS_UI_ENCODER
  EncoderColour firstColour = EC_NONE;
  EncoderColour secondColour = EC_NONE;
#endif
  bool updating = false;
  lv_display_t* disp;
  uint32_t* draw_buf;
  bool initialised = false;

  lv_obj_t* rpmLabel;
  lv_obj_t* pitchLabel;
  lv_obj_t* feedSymbolObj;
  lv_obj_t* leftStopObj;
  lv_obj_t* leftStopRectObj;
  lv_obj_t* rightStopObj;
  lv_obj_t* rightStopRectObj;

  lv_obj_t* pitchSlider;
  lv_obj_t* updateSlider;

  lv_obj_t* lockedObj;
  lv_obj_t* lockedRectObj;
  lv_obj_t* syncObj;
  lv_obj_t* syncRectObj;
  lv_obj_t* enableObj;
  lv_obj_t* enableRectObj;

  lv_obj_t* updateLabel;

  void initDisplay();
  void initialiseOta();

public:
  Display(Spindle* spindle, Leadscrew* leadscrew) {
    this->m_spindle = spindle;
    this->m_leadscrew = leadscrew;
    this->m_globalState = GlobalState::getInstance();
  }

  Display() {
    this->m_globalState = GlobalState::getInstance();
  }

  void init();
  void update();
  void showWifi(const char * ssid, const char * password, IPAddress ip);
  void showConnected(IPAddress ip);

protected:
  void initvars();
  void drawMode();
  void drawPitch();
  void drawEnabled();
  void drawLocked();
  void drawSpindleRpm();
  void drawStopStatus();
  void drawSyncStatus();
  void updateLed();
  void writeLed();
  void drawJogSpeed();
  void drawOTA();
};