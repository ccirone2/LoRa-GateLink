// config.cpp as the harnesses use it: its PARAMS table and lookups are the firmware's own; the three functions that
// reach a setting through a pointer to member (which ESBMC can't follow from a table) are replaced by the same logic
// by name (params.h, generated from the table), and the flash save by the harness's own (it only checks when it runs).
#pragma once
#include "env.h"
#include "params.h"
bool configSaveParamFlash(const ParamDef *p);
bool paramValidFirmware(const ParamDef *p, int32_t value);
bool paramSetFirmware(const ParamDef *p, int32_t value);
#define configSaveParam configSaveParamFlash
#define paramValid paramValidFirmware
#define paramSet paramSetFirmware
#include "config.cpp"
#undef configSaveParam
#undef paramValid
#undef paramSet

// As config.cpp's paramValid: in range, and bw_hz one of the three bandwidths.
bool paramValid(const ParamDef *p, int32_t value) {
  if (!p || value < p->minV || value > p->maxV) return false;
  if (p->id == PARAM_ID_bw_hz && value != 125000 && value != 250000 && value != 500000) return false;
  return true;
}

bool paramSet(const ParamDef *p, int32_t value) {
  if (!paramValid(p, value)) return false;
#define PARAM(pid, field, lo, hi, flags) if (p->id == (pid)) cfg.field = value;
  PARAMS_TABLE(PARAM)
#undef PARAM
  return true;
}

// Every setting anywhere in its range (bw_hz one of its three), and the role given: the proofs hold for all of them.
static void anyConfig(int32_t role) {
#define PARAM(pid, field, lo, hi, flags) cfg.field = nondet_i32(); ASSUME(cfg.field >= (lo) && cfg.field <= (hi));
  PARAMS_TABLE(PARAM)
#undef PARAM
  ASSUME(cfg.bw_hz == 125000 || cfg.bw_hz == 250000 || cfg.bw_hz == 500000);
  cfg.role = role;
  activeRole = role;
  cfg.key_set = 1;
}
