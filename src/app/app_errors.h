#pragma once

namespace app {

// App-owned diagnostic codes
enum class AppError : int {
    None = 0,

    IoThreadCreateFailed = 1001,
    IoThreadStartFailed = 1002,

    UsbBridgeLoadFailed = 1101,
    UsbHostStartFailed = 1102,
    UsbEnumerationFailed = 1103,
    UsbConnectTimedOut = 1104,
    UsbStopFailed = 1105,
    UsbSendRejected = 1106,
    UsbWriteFailed = 1107,
    UsbWriteTimedOut = 1108,
    UsbReadFailed = 1109,
    UsbStaleAccessory = 1110,

    AudioFormatUnsupported = 1201,
    AudioPortOpenFailed = 1202,
    AudioMutexCreateFailed = 1203,
    AudioSemaphoreCreateFailed = 1204,
    AudioThreadCreateFailed = 1205,
    AudioThreadStartFailed = 1206,
    AudioOutputFailed = 1207,

    MicrophoneSemaphoreCreateFailed = 1301,
    MicrophoneThreadCreateFailed = 1302,
    MicrophoneThreadStartFailed = 1303,
    MicrophonePortOpenFailed = 1304,
    MicrophoneCaptureFailed = 1305,
    MicrophoneSendFailed = 1306,

    InputThreadCreateFailed = 1401,
    InputThreadStartFailed = 1402,
    InputSendFailed = 1403,
    TouchPanelInfoFailed = 1404,
    TouchSamplingStartFailed = 1405,
    ControllerSamplingStartFailed = 1406,

    VideoQueueMutexCreateFailed = 1501,
    VideoPictureMutexCreateFailed = 1502,
    VideoSemaphoreCreateFailed = 1503,
    VideoThreadCreateFailed = 1504,
    VideoThreadStartFailed = 1505,
    VideoDecoderInitFailed = 1506,
    VideoDecodeFailed = 1507,

    GraphicsInitFailed = 1601,
    GraphicsShutdownFailed = 1602,
    GraphicsTextureCreateFailed = 1603,
    GraphicsTextureBindFailed = 1604,
    GraphicsUploadFailed = 1605,
    DisplayPresentFailed = 1606,

    AndroidAutoStartFailed = 1701,
    AndroidAutoFeedFailed = 1702,
    AndroidAutoSessionFailed = 1703,
    AndroidAutoStartTimedOut = 1704,

    ShellEventsInitFailed = 1801,
    ShellLockFailed = 1802,
    ShellUnlockFailed = 1803,

    NetInitFailed = 1901,
    NetNotConnected = 1902,
    NetNoGateway = 1903,
    NetSendRejected = 1904,
    NetWriteFailed = 1905,
    NetWriteTimedOut = 1906,
    NetReadFailed = 1907,
    NetPeerClosed = 1908,
};

inline const char* appErrorName(AppError error)
{
    switch (error) {
    case AppError::None: return "None";
    case AppError::IoThreadCreateFailed: return "IoThreadCreateFailed";
    case AppError::IoThreadStartFailed: return "IoThreadStartFailed";
    case AppError::UsbBridgeLoadFailed: return "UsbBridgeLoadFailed";
    case AppError::UsbHostStartFailed: return "UsbHostStartFailed";
    case AppError::UsbEnumerationFailed: return "UsbEnumerationFailed";
    case AppError::UsbConnectTimedOut: return "UsbConnectTimedOut";
    case AppError::UsbStopFailed: return "UsbStopFailed";
    case AppError::UsbSendRejected: return "UsbSendRejected";
    case AppError::UsbWriteFailed: return "UsbWriteFailed";
    case AppError::UsbWriteTimedOut: return "UsbWriteTimedOut";
    case AppError::UsbReadFailed: return "UsbReadFailed";
    case AppError::UsbStaleAccessory: return "UsbStaleAccessory";
    case AppError::AudioFormatUnsupported: return "AudioFormatUnsupported";
    case AppError::AudioPortOpenFailed: return "AudioPortOpenFailed";
    case AppError::AudioMutexCreateFailed: return "AudioMutexCreateFailed";
    case AppError::AudioSemaphoreCreateFailed: return "AudioSemaphoreCreateFailed";
    case AppError::AudioThreadCreateFailed: return "AudioThreadCreateFailed";
    case AppError::AudioThreadStartFailed: return "AudioThreadStartFailed";
    case AppError::AudioOutputFailed: return "AudioOutputFailed";
    case AppError::MicrophoneSemaphoreCreateFailed: return "MicrophoneSemaphoreCreateFailed";
    case AppError::MicrophoneThreadCreateFailed: return "MicrophoneThreadCreateFailed";
    case AppError::MicrophoneThreadStartFailed: return "MicrophoneThreadStartFailed";
    case AppError::MicrophonePortOpenFailed: return "MicrophonePortOpenFailed";
    case AppError::MicrophoneCaptureFailed: return "MicrophoneCaptureFailed";
    case AppError::MicrophoneSendFailed: return "MicrophoneSendFailed";
    case AppError::InputThreadCreateFailed: return "InputThreadCreateFailed";
    case AppError::InputThreadStartFailed: return "InputThreadStartFailed";
    case AppError::InputSendFailed: return "InputSendFailed";
    case AppError::TouchPanelInfoFailed: return "TouchPanelInfoFailed";
    case AppError::TouchSamplingStartFailed: return "TouchSamplingStartFailed";
    case AppError::ControllerSamplingStartFailed: return "ControllerSamplingStartFailed";
    case AppError::VideoQueueMutexCreateFailed: return "VideoQueueMutexCreateFailed";
    case AppError::VideoPictureMutexCreateFailed: return "VideoPictureMutexCreateFailed";
    case AppError::VideoSemaphoreCreateFailed: return "VideoSemaphoreCreateFailed";
    case AppError::VideoThreadCreateFailed: return "VideoThreadCreateFailed";
    case AppError::VideoThreadStartFailed: return "VideoThreadStartFailed";
    case AppError::VideoDecoderInitFailed: return "VideoDecoderInitFailed";
    case AppError::VideoDecodeFailed: return "VideoDecodeFailed";
    case AppError::GraphicsInitFailed: return "GraphicsInitFailed";
    case AppError::GraphicsShutdownFailed: return "GraphicsShutdownFailed";
    case AppError::GraphicsTextureCreateFailed: return "GraphicsTextureCreateFailed";
    case AppError::GraphicsTextureBindFailed: return "GraphicsTextureBindFailed";
    case AppError::GraphicsUploadFailed: return "GraphicsUploadFailed";
    case AppError::DisplayPresentFailed: return "DisplayPresentFailed";
    case AppError::AndroidAutoStartFailed: return "AndroidAutoStartFailed";
    case AppError::AndroidAutoFeedFailed: return "AndroidAutoFeedFailed";
    case AppError::AndroidAutoSessionFailed: return "AndroidAutoSessionFailed";
    case AppError::AndroidAutoStartTimedOut: return "AndroidAutoStartTimedOut";
    case AppError::ShellEventsInitFailed: return "ShellEventsInitFailed";
    case AppError::ShellLockFailed: return "ShellLockFailed";
    case AppError::ShellUnlockFailed: return "ShellUnlockFailed";
    case AppError::NetInitFailed: return "NetInitFailed";
    case AppError::NetNotConnected: return "NetNotConnected";
    case AppError::NetNoGateway: return "NetNoGateway";
    case AppError::NetSendRejected: return "NetSendRejected";
    case AppError::NetWriteFailed: return "NetWriteFailed";
    case AppError::NetWriteTimedOut: return "NetWriteTimedOut";
    case AppError::NetReadFailed: return "NetReadFailed";
    case AppError::NetPeerClosed: return "NetPeerClosed";
    }
    return "UnknownAppError";
}

} // namespace app
