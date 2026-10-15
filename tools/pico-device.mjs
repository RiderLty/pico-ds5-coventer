// ES module entry point; the classic build also supports file:// script tags.
import "./pico-device.js";
export const { PicoDevice, PicoProtocol, PicoError, PicoDeviceError } = globalThis;
export default PicoDevice;
