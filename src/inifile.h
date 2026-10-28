/*
  inifile.h
 */

#ifndef INIFILE_H
#define INIFILE_H

#include <stdbool.h>
#include <stdint.h>

#define INIFILE_OPTION_HOTKEY   0   // HID key code
#define INIFILE_OPTION_LED      1   // 0 = blink, 1 = on, 0 = off
#define INIFILE_OPTION_PADMENU  2   // gamepad menu button: 0 = off, 1 = Select+Start, 2 = Start, 3 = Select
#define INIFILE_OPTION_VOLKEYS  3   // volume keys: 0 = off, 1 = F9/F10, 2 = Ctrl -/=
#define INIFILE_OPTION_MEDIAKEYS 4  // keyboard media volume keys: 0 = off, 1 = on
#define INIFILE_OPTION_PADVOL   5   // gamepad volume with Select + L/R: 0 = off, 1 = on
#define INIFILE_OPTIONS         6

// handle the core specific ini file
int inifile_read(char *);
int inifile_write(char *);   // 0 = written, -1 = failed
int inifile_option_get(int id);
void inifile_option_set(int id, int value);

// handle the global ini file (config.ini)
void inifile_config_read(void);
bool inifile_config_is_read(void);  // true once config.ini has been processed
void inifile_config_wait(void);     // blocks until then, one waiting task at a time
bool inifile_config_has(char *section, char *name);
uint32_t inifile_config_get_ip_n(char *section, char *name, uint32_t def, int n);
uint32_t inifile_config_get_ip(char *section, char *name, uint32_t def);
int inifile_config_get_int(char *section, char *name, int def);
char *inifile_config_get_str(char *section, char *name);
int inifile_config_num_values(char *section, char *name);

#endif // INIFILE_H
