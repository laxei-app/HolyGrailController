package com.canon.ccapisample;

import java.util.HashMap;
import java.util.Map;

public final class Constants {
    static final class CCAPI {
        static final String VER100 = "ver100";
        static final String VER110 = "ver110";
        static final String VER120 = "ver120";
        static final String VER130 = "ver130";
        static final String[] SUPPORTED_API_VERSION = { VER100, VER110, VER120, VER130 };

        static final int DIR_PATH_DEPTH = 5;

        static final class Method {
            static final String GET = "GET";
            static final String PUT = "PUT";
            static final String POST = "POST";
            static final String DELETE = "DELETE";
        }

        static final class Key{
            static final String TOPURLFORDEV = "topurlfordev";
            static final String DEVICESTATUS_STORAGE = "devicestatus/storage";
            static final String FUNCTIONS_SSL_CACERT = "functions/ssl/cacert";
            static final String FUNCTIONS_DATETIME = "functions/datetime";
            static final String FUNCTIONS_WIFISETTING = "functions/wifisetting";
            static final String FUNCTIONS_NETWORKSETTING = "functions/networksetting";
            static final String FUNCTIONS_CURRENTCONNECTSETTING = "functions/networksetting/currentconnectionsetting";
            static final String FUNCTIONS_CONNECTSETTING = "functions/networksetting/connectionsetting";
            static final String FUNCTIONS_COMMSETTING = "functions/networksetting/commsetting";
            static final String FUNCTIONS_FUNCTIONSETTING = "functions/networksetting/functionsetting";
            static final String FUNCTIONS_CARDFORMAT = "functions/cardformat";
            static final String FUNCTIONS_SENSORCLEANING = "functions/sensorcleaning";
            static final String FUNCTIONS_WIFICONNECTION = "functions/wificonnection";
            static final String FUNCTIONS_NETWORKCONNECTION = "functions/networkconnection";
            static final String CONTENTS = "contents";
            static final String SHOOTING_CONTROL_SHOOTINGMODE = "shooting/control/shootingmode";
            static final String SHOOTING_CONTROL_SHUTTERBUTTON = "shooting/control/shutterbutton";
            static final String SHOOTING_CONTROL_SHUTTERBUTTON_MANUAL = "shooting/control/shutterbutton/manual";
            static final String SHOOTING_CONTROL_MOVIEMODE = "shooting/control/moviemode";
            static final String SHOOTING_CONTROL_IGNORESHOOTINGMODEDIALMODE = "shooting/control/ignoreshootingmodedialmode";
            static final String SHOOTING_CONTROL_RECBUTTON = "shooting/control/recbutton";
            static final String SHOOTING_CONTROL_ZOOM = "shooting/control/zoom";
            static final String SHOOTING_CONTROL_DRIVEFOCUS = "shooting/control/drivefocus";
            static final String SHOOTING_CONTROL_AF = "shooting/control/af";
            static final String SHOOTING_SETTINGS_ANTIFLICKERSHOOT = "shooting/settings/antiflickershoot";
            static final String SHOOTING_SETTINGS_HFANTIFLICKERSHOOT = "shooting/settings/hfantiflickershoot";
            static final String SHOOTING_CONTROL_FLICKERDETECTION = "shooting/control/flickerdetection";
            static final String SHOOTING_CONTROL_HFFLICKERDETECTION = "shooting/control/hfflickerdetection";
            static final String SHOOTING_SETTINGS_HFFLICKERTV = "shooting/settings/hfflickertv";
            static final String SHOOTING_CONTROL_HFFLICKERTV = "shooting/control/hfflickertv";
            static final String SHOOTING_LIVEVIEW = "shooting/liveview";
            static final String SHOOTING_LIVEVIEW_FLIP = "shooting/liveview/flip";
            static final String SHOOTING_LIVEVIEW_FLIPDETAIL = "shooting/liveview/flipdetail";
            static final String SHOOTING_LIVEVIEW_SCROLL = "shooting/liveview/scroll";
            static final String SHOOTING_LIVEVIEW_SCROLLDETAIL = "shooting/liveview/scrolldetail";
            static final String SHOOTING_LIVEVIEW_RTPSESSIONDESC = "shooting/liveview/rtpsessiondesc";
            static final String SHOOTING_LIVEVIEW_RTP = "shooting/liveview/rtp";
            static final String SHOOTING_LIVEVIEW_ANGLEINFORMATION = "shooting/liveview/angleinformation";
            static final String SHOOTING_LIVEVIEW_AFFRAMEPOSITION = "shooting/liveview/afframeposition";
            static final String SHOOTING_LIVEVIEW_CLICKWB = "shooting/liveview/clickwb";
            static final String SHOOTING_OPTICALFINDER_AFAREASELECTIONMODE = "shooting/opticalfinder/afareaselectionmode";
            static final String SHOOTING_OPTICALFINDER_AFAREASELECTION = "shooting/opticalfinder/afareaselection";
            static final String SHOOTING_OPTICALFINDER_AFFRAMEINFORMATION = "shooting/opticalfinder/afframeinformation";
            static final String SHOOTING_OPTICALFINDER_AFAREAINFORMATION = "shooting/opticalfinder/afareainformation";
            static final String EVENT_POLLING = "event/polling";
            static final String EVENT_MONITORING = "event/monitoring";
        }

        static final class Field {
            static final String MESSAGE = "message";
            static final String URL_LIST = "url";
            static final String VALUE = "value";
            static final String ABILITY = "ability";
            static final String ACTION = "action";
            static final String STATUS = "status";
            static final String MIN = "min";
            static final String MAX = "max";
            static final String STEP = "step";
            static final String RESULT = "result";
            static final String NG = "ng";

            static final String MOVIEMODE = "moviemode";
            static final String IGNORESHOOTINGMODEDIALMODE = "ignoreshootingmodedialmode";
            static final String RECBUTTON = "recbutton";
            static final String ANTIFLICKERSHOOT = "antiflickershoot";
            static final String HFANTIFLICKERSHOOT = "hfantiflickershoot";
            static final String HFFLICKETV = "hfflickertv";
            static final String LIVEVIEW = "liveview";
            static final String LIVEVIEWSIZE = "liveviewsize";
            static final String CAMERADISPLAY = "cameradisplay";
            static final String AFAREASELECTIONMODE = "afareaselectionmode";
            static final String AFAREASELECTION = "afareaselection";

            static final String FREQUENCY = "frequency";
            static final String TV = "tv";

            static final String HISTOGRAM = "histogram";
            static final String AF_FRAME = "afframe";
            static final String IMAGE = "image";
            static final String VISIBLE = "visible";
            static final String ZOOM = "zoom";

            static final String POSITION_X = "positionx";
            static final String POSITION_Y = "positiony";
            static final String POSITION_WIDTH = "positionwidth";
            static final String POSITION_HEIGHT = "positionheight";
            static final String MAGNIFICATION = "magnification";

            static final String SELECT = "select";
            static final String X = "x";
            static final String Y = "y";
            static final String WIDTH = "width";
            static final String HEIGHT = "height";

            static final String AF_MODE_AUTO = "auto";
            static final String AF_MODE_ZONE = "zone";
            static final String AF_MODE_LARGE_ZONE = "large_zone";
            static final String AF_AREA = "afarea";

            static final String BATTERY = "battery";
            static final String BATTERY_LIST = "batterylist";

            static final String CONTENTS_NUMBER = "contentsnumber";
            static final String PAGE_NUMBER = "pagenumber";
            static final String ADDED_CONTENTS = "addedcontents";
            static final String DELETED_CONTENTS = "deletedcontents";
            static final String STORAGE = "storage";
            static final String STORAGE_LIST = "storagelist";
            static final String STORAGE_NAME = "name";
            static final String STORAGE_URL = "url";
            static final String STORAGE_PATH = "path";

            static final String DATETIME = "datetime";
            static final String DST = "dst";

            static final String RECORDFUNCTIONS = "recordfunctions";
            static final String CARDSELECTION = "cardselection";

            static final String EXPOSURE_INCREMENTS = "exposureincrements";

            static final String SSL_CACERT = "ssl/cacert";

            static final String WIFI_SETTINGS = "wifisetting";
            static final String WIFI_SETTINGS_SET_1 = "wifisetting/set1";
            static final String WIFI_SETTINGS_SET_2 = "wifisetting/set2";
            static final String WIFI_SETTINGS_SET_3 = "wifisetting/set3";

            static final String SSID = "ssid";
            static final String METHOD = "method";
            static final String CHANNEL = "channel";
            static final String AUTHENTICATION = "authentication";
            static final String ENCRYPTION = "encryption";
            static final String KEYINDEX = "keyindex";
            static final String PASSWORD = "password";
            static final String IPADDRESSSET = "ipaddressset";
            static final String IPADDRESS = "ipaddress";
            static final String SUBNETMASK = "subnetmask";
            static final String GATEWAY = "gateway";

            static final String LANTYPE = "lantype";
            static final String IPV4_IPADDRESSSET = "ipv4_ipaddressset";
            static final String IPV4_IPADDRESS = "ipv4_ipaddress";
            static final String IPV4_SUBNETMASK = "ipv4_subnetmask";
            static final String IPV4_GATEWAY = "ipv4_gateway";
            static final String IPV6_USEIPV6 = "ipv6_useipv6";
            static final String IPV6_MANUAL_SETTING = "ipv6_manual_setting";
            static final String IPV6_MANUAL_ADDRESS = "ipv6_manual_address";
            static final String IPV6_PREFIXLENGTH = "ipv6_prefixlength";
            static final String IPV6_GATEWAY = "ipv6_gateway";

            static final String NW = "nw";
            static final String MODE = "mode";

            static final String COMMSETTING = "commsetting";
            static final String COMMSETTING1 = "commsetting1";
            static final String COMMSETTING2 = "commsetting2";
            static final String FUNCTIONSETTING = "functionsetting";
            static final String FUNCTIONSETTING1 = "functionsetting1";
            static final String FUNCTIONSETTING2 = "functionsetting2";

            static final String COMMFUNCTION = "commfunction";

            static final String CARD_FORMAT = "cardformat";
            static final String CONTENTS_URL = "url";
            static final String CONTENTS_PATH = "path";

            static final String SENSORCLEANING = "sensorcleaning";
            static final String AUTOPOWEROFF = "autopoweroff";

            static final String SUPPLEMENT = "supplement";

            static final String NO_LIST_OF_APIS = "No list of APIs";
        }

        static final class Value {
            static final String ON = "on";
            static final String OFF = "off";

            static final String DISABLE = "disable";
            static final String ENABLE = "enable";

            static final String MODE_NOT_SUPPORTED = "Mode not supported";

            static final String INFRASTRUCTURE = "infrastructure";
            static final String CAMERAAP = "cameraap";
            static final String OPEN = "open";
            static final String SHAREDKEY = "sharedkey";
            static final String WPAWPA2PSK = "wpawpa2psk";
            static final String WPAWPA2WPA3PERSONAL = "wpawpa2wpa3personal";
            static final String NONE = "none";
            static final String WEP = "wep";
            static final String TKIPAES = "tkipaes";
            static final String AES = "aes";
            static final String AUTO = "auto";
            static final String MANUAL = "manual";

            static final String ROTATE = "rotate";
            static final String PROTECT = "protect";
            static final String ARCHIVE = "archive";
            static final String RATING = "rating";
            static final String GPS = "gps";
            static final String XMP = "xmp_description";

            static final int NOT_SELECTED = 0x00;
            static final int SELECTED = 0x01;
            static final int NOT_SELECTABLE = 0x02;

            static final String SET = "set";
            static final String EDIT = "EDIT";
        }

        static final Map<String, String> UNIT_MAP = new HashMap<String, String>() {
            {  put("remainingtime", "sec");  }
            {  put("filesize", "byte");  }
            {  put("playtime", "sec");  }
        };
    }
    public enum RequestCode {
        ACT_WEB_API,
        GET_DEVICESTATUS_STORAGE,
        GET_FUNCTIONS_DATETIME,
        PUT_FUNCTIONS_DATETIME,
        GET_FUNCTIONS_WIFISETTINGINFORMATION,
        PUT_FUNCTIONS_WIFISETTINGINFORMATION,
        DELETE_FUNCTIONS_WIFISETTINGINFORMATION,
        GET_FUNCTIONS_NETWORKSETTING,
        GET_FUNCTIONS_CURRENTCONNECT_SETTING,
        PUT_FUNCTIONS_CURRENTCONNECT_SETTING,
        GET_FUNCTIONS_CONNECT_SETTING,
        GET_FUNCTIONS_COMM_SETTING,
        GET_FUNCTIONS_FUNCTION_SETTING,
        POST_FUNCTIONS_CARDFORMAT,
        POST_FUNCTIONS_SENSORCLEANING,
        POST_FUNCTIONS_WIFICONNECTION,
        POST_FUNCTIONS_NETWORKCONNECTION,
        GET_SHOOTING_CONTROL_SHOOTINGMODE,
        POST_SHOOTING_CONTROL_SHOOTINGMODE,
        POST_SHOOTING_CONTROL_SHUTTERBUTTON,
        POST_SHOOTING_CONTROL_SHUTTERBUTTON_MANUAL,
        POST_SHOOTING_CONTROL_RECBUTTON,
        GET_SHOOTING_CONTROL_MOVIEMODE,
        POST_SHOOTING_CONTROL_MOVIEMODE,
        GET_SHOOTING_CONTROL_IGNORESHOOTINGMODEDIALMODE,
        POST_SHOOTING_CONTROL_IGNORESHOOTINGMODEDIALMODE,
        GET_SHOOTING_CONTROL_ZOOM,
        POST_SHOOTING_CONTROL_ZOOM,
        POST_SHOOTING_CONTROL_DRIVEFOCUS,
        POST_SHOOTING_CONTROL_AF,
        GET_SHOOTING_SETTINGS_ANTIFLICKERSHOOT,
        PUT_SHOOTING_SETTINGS_ANTIFLICKERSHOOT,
        GET_SHOOTING_SETTINGS_HFANTIFLICKERSHOOT,
        PUT_SHOOTING_SETTINGS_HFANTIFLICKERSHOOT,
        GET_SHOOTING_SETTINGS_HFFLICKERTV,
        PUT_SHOOTING_SETTINGS_HFFLICKERTV,
        POST_SHOOTING_CONTROL_FLICKERDETECTION,
        POST_SHOOTING_CONTROL_HFFLICKERDETECTION,
        POST_SHOOTING_CONTROL_HFFLICKERTV,
        POST_SHOOTING_LIVEVIEW,
        GET_SHOOTING_LIVEVIEW_FLIP,
        GET_SHOOTING_LIVEVIEW_FLIPDETAIL,
        GET_SHOOTING_LIVEVIEW_SCROLL,
        DELETE_SHOOTING_LIVEVIEW_SCROLL,
        GET_SHOOTING_LIVEVIEW_SCROLLDETAIL,
        DELETE_SHOOTING_LIVEVIEW_SCROLLDETEIL,
        GET_SHOOTING_LIVEVIEW_RTPSESSIONDESC,
        POST_SHOOTING_LIVEVIEW_RTP,
        POST_SHOOTING_LIVEVIEW_ANGLEINFORMATION,
        PUT_SHOOTING_LIVEVIEW_AFFRAMEPOSITION,
        POST_SHOOTING_LIVEVIEW_CLICKWB,
        GET_SHOOTING_OPTICALFINDER_AFAREASELECTIONMODE,
        GET_SHOOTING_OPTICALFINDER_AFAREASELECTION,
        PUT_SHOOTING_OPTICALFINDER_AFAREASELECTION,
        GET_SHOOTING_OPTICALFINDER_AFFRAMEINFORMATION,
        GET_SHOOTING_OPTICALFINDER_AFAREAINFORMATION,
        GET_EVENT_POLLING,
        DELETE_EVENT_POLLING,
        GET_EVENT_MONITORING,
        DELETE_EVENT_MONITORING,
    }
    enum EventMethod{
        POLLING,
        POLLING_CONTINUE,
        POLLING_SHORT,
        POLLING_LONG,
        POLLING_IMMEDIATELY,
        MONITORING
    }
    enum LiveViewMethod{
        FLIP,
        FLIPDETAIL,
        SCROLL,
        SCROLLDETAIL
    }
    enum LiveViewKind{
        IMAGE,
        IMAGE_AND_INFO,
        INFO
    }
    enum WifiMonitoringResult{
        CONNECTION,
        DISCONNECTION,
        INTERRUPT
    }
    static final class Settings {
        static final class FileName{
            static final String SETTINGS = "settings.json";
        }
        static final class Key{
            static final String SHOOTING_SETTINGS = "ShootingSettings";
            static final String DEVICE_INFORMATION = "DeviceInformation";
            static final String CAMERA_FUNCTIONS = "Functions";
            static final String NAME = "Name";
            static final String KEY = "Key";
        }
    }
    static final class RemoteCapture{
        static final String LV_SIZE_OFF = "off";
        static final String LV_SIZE_SMALL = "small";
        static final String LV_SIZE_MEDIUM = "medium";
        static final String LV_DISPLAY_ON = "on";
        static final String LV_DISPLAY_OFF = "off";
        static final String LV_DISPLAY_KEEP = "keep";
        static final String LV_OFF_ON = LV_SIZE_OFF + "/" + LV_DISPLAY_ON;
        static final String LV_OFF_OFF = LV_SIZE_OFF + "/" + LV_DISPLAY_OFF;
        static final String LV_OFF_KEEP = LV_SIZE_OFF + "/" + LV_DISPLAY_KEEP;
        static final String LV_SMALL_ON = LV_SIZE_SMALL + "/" + LV_DISPLAY_ON;
        static final String LV_SMALL_OFF = LV_SIZE_SMALL + "/" + LV_DISPLAY_OFF;
        static final String LV_SMALL_KEEP = LV_SIZE_SMALL + "/" + LV_DISPLAY_KEEP;
        static final String LV_MIDDLE_ON = LV_SIZE_MEDIUM + "/" + LV_DISPLAY_ON;
        static final String LV_MIDDLE_OFF = LV_SIZE_MEDIUM + "/" + LV_DISPLAY_OFF;
        static final String LV_MIDDLE_KEEP = LV_SIZE_MEDIUM + "/" + LV_DISPLAY_KEEP;
        static final String[] LV_ARRAY = {LV_SMALL_ON, LV_SMALL_OFF, LV_SMALL_KEEP, LV_MIDDLE_ON, LV_MIDDLE_OFF, LV_MIDDLE_KEEP, LV_OFF_ON, LV_OFF_OFF, LV_OFF_KEEP};

        static final int FINEDER_WIDTH = 960;
        static final int FINEDER_HEIGHT = 640;
    }
    static final class ContentsViewer{
        static final String SAVE_ORIGINAL = "original";
        static final String SAVE_DISPLAY = "display";
        static final String SAVE_EMBEDDED = "embedded";
        static final String[] SAVE_ARRAY = {SAVE_ORIGINAL, SAVE_DISPLAY, SAVE_EMBEDDED};
    }
}
