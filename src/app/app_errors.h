#pragma once

namespace app {

#define VITA_AUTO_APP_ERRORS(X) \
    X(None, 0) \
    \
    X(IoThreadCreateFailed, 1001) \
    X(IoThreadStartFailed, 1002) \
    \
    X(UsbBridgeLoadFailed, 1101) \
    X(UsbHostStartFailed, 1102) \
    X(UsbEnumerationFailed, 1103) \
    X(UsbConnectTimedOut, 1104) \
    X(UsbStopFailed, 1105) \
    X(UsbSendRejected, 1106) \
    X(UsbWriteFailed, 1107) \
    X(UsbWriteTimedOut, 1108) \
    X(UsbReadFailed, 1109) \
    X(UsbStaleAccessory, 1110) \
    \
    X(AudioFormatUnsupported, 1201) \
    X(AudioPortOpenFailed, 1202) \
    X(AudioMutexCreateFailed, 1203) \
    X(AudioSemaphoreCreateFailed, 1204) \
    X(AudioThreadCreateFailed, 1205) \
    X(AudioThreadStartFailed, 1206) \
    X(AudioOutputFailed, 1207) \
    \
    X(MicrophoneSemaphoreCreateFailed, 1301) \
    X(MicrophoneThreadCreateFailed, 1302) \
    X(MicrophoneThreadStartFailed, 1303) \
    X(MicrophonePortOpenFailed, 1304) \
    X(MicrophoneCaptureFailed, 1305) \
    X(MicrophoneSendFailed, 1306) \
    \
    X(InputThreadCreateFailed, 1401) \
    X(InputThreadStartFailed, 1402) \
    X(InputSendFailed, 1403) \
    X(TouchPanelInfoFailed, 1404) \
    X(TouchSamplingStartFailed, 1405) \
    X(ControllerSamplingStartFailed, 1406) \
    \
    X(VideoQueueMutexCreateFailed, 1501) \
    X(VideoPictureMutexCreateFailed, 1502) \
    X(VideoSemaphoreCreateFailed, 1503) \
    X(VideoThreadCreateFailed, 1504) \
    X(VideoThreadStartFailed, 1505) \
    X(VideoDecoderInitFailed, 1506) \
    X(VideoDecodeFailed, 1507) \
    \
    X(GraphicsInitFailed, 1601) \
    X(GraphicsShutdownFailed, 1602) \
    X(GraphicsTextureCreateFailed, 1603) \
    X(GraphicsTextureBindFailed, 1604) \
    X(GraphicsUploadFailed, 1605) \
    X(DisplayPresentFailed, 1606) \
    \
    X(AndroidAutoStartFailed, 1701) \
    X(AndroidAutoFeedFailed, 1702) \
    X(AndroidAutoSessionFailed, 1703) \
    X(AndroidAutoStartTimedOut, 1704) \
    \
    X(ShellEventsInitFailed, 1801) \
    X(ShellLockFailed, 1802) \
    X(ShellUnlockFailed, 1803) \
    \
    X(NetInitFailed, 1901) \
    X(NetNotConnected, 1902) \
    X(NetNoGateway, 1903) \
    X(NetSendRejected, 1904) \
    X(NetWriteFailed, 1905) \
    X(NetWriteTimedOut, 1906) \
    X(NetReadFailed, 1907) \
    X(NetPeerClosed, 1908) \
    \
    X(CarplayStartFailed, 2001) \
    X(WifiApFailed, 2002)

enum class AppError : int {
#define VITA_AUTO_APP_ERROR_VALUE(name, code) name = code,
    VITA_AUTO_APP_ERRORS(VITA_AUTO_APP_ERROR_VALUE)
#undef VITA_AUTO_APP_ERROR_VALUE
};

inline const char* appErrorName(AppError error)
{
    switch (error) {
#define VITA_AUTO_APP_ERROR_NAME(name, code) case AppError::name: return #name;
    VITA_AUTO_APP_ERRORS(VITA_AUTO_APP_ERROR_NAME)
#undef VITA_AUTO_APP_ERROR_NAME
    }
    return "UnknownAppError";
}

} // namespace app
