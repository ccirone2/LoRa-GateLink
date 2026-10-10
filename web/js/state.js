// What the console knows about the connected board, shared by its modules (one object, so every module sees the
// same values and may change them).
export const S = {
  port: null, // the open console port; null = not connected
  role: 'unset',
  keySet: false,
  meta: [], // config.get meta: per param name, id, min, max, radio, remote, reboot
  params: {}, // config.get params: name -> value, as the board runs them
  appliedUnsaved: false, // settings applied since the last Save (this page's own record; lost on reboot)
  boardInfo: null, // last info reply
  rebootPending: false, // last status: a saved role waits for a reboot
  flashing: false,
  flashCheck: null, // what the board should report once it's back on new firmware
  latestFw: null, // firmware/latest.json, written by the Pages deploy from the latest release
};
