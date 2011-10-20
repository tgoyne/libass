#define _CRT_SECURE_NO_WARNINGS
#define _CRT_NONSTDC_NO_DEPRECATE

//#define CONFIG_ENCA 1
#define CONFIG_FONTCONFIG 1
#define CONFIG_ICONV 1

#define inline __inline

#define strtoll(p, e, b) _strtoi64(p, e, b)

#define M_PI 3.1415926535897932384626433832795
