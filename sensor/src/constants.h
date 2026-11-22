// Default button codes (9-byte FB frames). They can be changed from the web page
// (Réglages → Codes des touches) and are then stored in the module: see keys.h.
#define DEF_COMMAND_UP          "fb0603450e0001fe66"
#define DEF_COMMAND_DOWN        "fb0603450e0002fd50"
//#define COMMAND_TOGGLE      "fb066666666609f6c2"
#define DEF_COMMAND_TIME        "fb0664d4060003fc76"
#define DEF_COMMAND_CHANGE_MODE "fb0603450e0004fb3c"
#define DEF_COMMAND_AUX         "fb0603450e000af5c0"
#define DEF_COMMAND_JET1        "fb060343060006f9a2" // TODO: fix me - swapped to tmjo's panel as mine was one char short in my notes
#define DEF_COMMAND_JET2        "fb060343060007f8b0" // TODO: fix me - swapped to tmjo's panel as mine was one char short in my notes
#define DEF_COMMAND_JET3        ""                   // unknown: capture it from the panel (Jets 3)
#define DEF_COMMAND_LIGHT       "fb0603450e0009f6f6"
#define COMMAND_EMPTY           "fb0603450e0000ff74"

enum KeyId { K_UP, K_DOWN, K_MODE, K_LIGHT, K_JET1, K_JET2, K_JET3, K_AUX, K_TIME, K_COUNT };
extern String keyCode[K_COUNT];
#define COMMAND_UP          keyCode[K_UP]
#define COMMAND_DOWN        keyCode[K_DOWN]
#define COMMAND_TIME        keyCode[K_TIME]
#define COMMAND_CHANGE_MODE keyCode[K_MODE]
#define COMMAND_AUX         keyCode[K_AUX]
#define COMMAND_JET1        keyCode[K_JET1]
#define COMMAND_JET2        keyCode[K_JET2]
#define COMMAND_JET3        keyCode[K_JET3]
#define COMMAND_LIGHT       keyCode[K_LIGHT]

// Std;Eco;Sleep
#define MODE_IDX_STD 0
#define MODE_IDX_ECO 1
#define MODE_IDX_SLP 2

#ifdef PUMP1_DUAL_SPEED
#define PUMP1_STATE_HIGH 2
#else
#define PUMP1_STATE_HIGH 1
#endif 

#ifdef PUMP2_DUAL_SPEED
#define PUMP2_STATE_HIGH 2
#else
#define PUMP2_STATE_HIGH 1
#endif 

#ifdef AUX_DUAL_SPEED
#define AUX_STATE_HIGH 2
#else
#define AUX_STATE_HIGH 1
#endif 

#define STATUS_BOOT 0
#define STATUS_WIFI 2
#define STATUS_OK 1
#define STATUS_WAITING_DATA 3
#define STATUS_WAITING_PANEL 4
