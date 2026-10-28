/*
  menu.c - MiSTeryNano menu based in u8g2

  This version includes the old static MiSTeryNano type of menu
  as well as the new config driven one.

*/
  
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <ff.h>
#include <diskio.h>

#ifdef ESP_PLATFORM
#include <freertos/FreeRTOS.h>
#include <freertos/timers.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#else
#include <FreeRTOS.h>
#include <timers.h>
#include <task.h>
#include <queue.h>
#endif

#include "sdc.h"
#include "osd.h"
#include "inifile.h"
#include "menu.h"
#include "sysctrl.h"
#include "debug.h"
#include "mcu_hw.h"

#ifdef ENABLE_BLUETOOTH
#include "bluetooth.h"
#endif

// this is the u8g2_font_helvR08_te with any trailing
// spaces removed
#include "font_helvR08_te.c"

// some constants for arrangement
// The OSD (currently) is 64 pixel high. To allow for a proper
// box around a text line, it needs to be 12 pixels high. A total
// of five lines is 5*12 = 60 + title seperation line
#define MENU_LINE_Y      13   // y pos of seperator line
#define MENU_ENTRY_H     12   // height of regular menu entries
#define MENU_ENTRY_BASE   9   // font baseline offset


#define MENU_FORM_FSEL           -1

#define MENU_ENTRY_INDEX_ID       0
#define MENU_ENTRY_INDEX_LABEL    1
#define MENU_ENTRY_INDEX_FORM     2
#define MENU_ENTRY_INDEX_OPTIONS  2
#define MENU_ENTRY_INDEX_VARIABLE 3

// menu types
#define MENU_TYPE_MENU          0
#define MENU_TYPE_FILESELECTOR  1
#define MENU_TYPE_CUSTOM        2  // only internally used

typedef struct config_custom_S {
  char *label;

  // Pointer to function the returns the number of active entries in
  // this custom dialog. This is the number of "lines" that can be
  // selected with the up and down keys
  int (*length)(void); 
  
  // Pointer to function that draws the custom contents
  void (*draw)(void); 
  
} config_custom_t;

/* new menu state */
typedef struct menu_state {
  int type;
  int selected; 
  int scroll;
  union {
    const config_menu_t *menu;  
    const char *label;             // used for file/image selector, only
    const config_custom_t *custom;  
  };
  
  // file selector related
  sdc_dir_entry_t *dir;

  struct menu_state *prev;
} menu_state_t;

typedef struct {
  char index;
  char **ext;
  config_action_t *action;  // action to be run afterwards

  // string and icon to be used if nothing is selected
  // by default this is "[X] No Disk"
  char *none_str;
  unsigned char *none_icn;
} fsel_state_t;

// menu state is a stack since as the menu is hierarchical
static menu_state_t *menu_state = NULL;
static fsel_state_t fsel_state;

// settings changed through the menu or hotkeys since the last save
static bool menu_dirty = false;

// "Apply and restart?" question, asked when leaving a page with waiting changes
static struct {
  bool open;
  int sel;      // 0 = Apply, 1 = Discard
  bool close;   // close the menu afterwards, else go back one page
} confirm;

// hidden lists (hidden="true") are kept and saved, but not shown
static bool menu_entry_visible(const config_menu_entry_t *e) {
  return !(e->type == CONFIG_MENU_ENTRY_LIST && (e->list->flags & CONFIG_LIST_HIDDEN));
}

// n-th visible entry of a menu
static config_menu_entry_t *menu_entry_nth(const config_menu_t *menu, int n) {
  for(config_menu_entry_t *e = menu->entries; e; e = e->next)
    if(menu_entry_visible(e) && n-- == 0) return e;
  return NULL;
}

/* =========== handling of variables ============= */
static menu_variable_t *variables = NULL;

menu_variable_t *menu_get_variables(void) {
  return variables;
}

static int menu_variable_get(char id) {
  menu_variable_t *v = variables;

  while(v) {
    if(v->id == id) return v->value;
    v = v->next;
  }
  return 0;  
}

static bool menu_variable_exists(char id) {
  menu_variable_t *v = variables;

  while(v) {
    if(v->id == id) return true;
    v = v->next;
  }
  return false;  
}

static void menu_variable_set(char id, int value) {
  menu_variable_t *v = variables;
  
  while(v) {
    if(v->id == id) {
      if(v->value != value) {
	v->value = value;
	// also set this in the core
	sys_set_val(id, value);
      }
    }
    v = v->next;
  }
}

static void menu_setup_variable(char id, int value) {
  // menu_debugf("setup variable '%c' = %d", id, value);

  // allocate new entry
  menu_variable_t *variable = pvPortMalloc(sizeof(menu_variable_t));
  variable->id = id;
  variable->value = value;
  variable->next = NULL;

  if(!variables)
    variables = variable;
  else {
    menu_variable_t *v = variables;
    while(v->next) v = v->next;
    v->next = variable;
  }

  // also set the value in the core
  sys_set_val(id, value);
}

static void menu_setup_menu_variables(const config_menu_t *menu) {
  config_menu_entry_t *me = menu->entries;
  while(me) {
    if(me->type == CONFIG_MENU_ENTRY_MENU)
      menu_setup_menu_variables(me->menu);
    
    if(me->type == CONFIG_MENU_ENTRY_LIST) {
      // setup variable ...
      menu_setup_variable(me->list->id, me->list->def);
    }
    
    if(me->type == CONFIG_MENU_ENTRY_TOGGLE) {
      // setup variable ...
      menu_setup_variable(me->toggle->id, me->toggle->def);
    }
    
    if(me->type == CONFIG_MENU_ENTRY_RANGE) {
      // setup variable ...
      menu_setup_variable(me->range->id, me->range->def);
    }
    
    me = me->next;
  }
}

static void menu_setup_variables(void) {
  // variables occur in two places:
  // in the set command used in actions
  // in menu items (currently only in lists as buttons use actions)

  // Only the variables from menu items are actually stored permanently.
  // Action variables are sent into the core and processed there.
  
  // search through menu tree for lists
  menu_setup_menu_variables(cfg->menu);  
}


void menu_set_value(unsigned char id, int8_t value) {
  // this is called when reading the ini file
  // We allow values in the ini file which are actually not in the
  // menu at all. This can be used for "static" changes which are made
  // once by manually manipulating the ini file e.g. with a text
  // editor

  if(!menu_variable_exists(id)) {
    // previous companion versions were writing the action related variables
    // into the ini file as well. This mainly affected the 'R' variable often
    // used for reset. We ignore this particular variable by now as this was
    // not intentinal. This should be removed one day.
    if(id == 'R') {
      menu_debugf("suppressing 'R' reset variable");
      return;
    }
    menu_setup_variable(id, value);
  } else
    menu_variable_set(id, value);
}

// various 8x8 icons
static const unsigned char icn_right_bits[]  = { 0x00,0x04,0x0c,0x1c,0x3c,0x1c,0x0c,0x04 };
static const unsigned char icn_left_bits[]   = { 0x00,0x20,0x30,0x38,0x3c,0x38,0x30,0x20 };
static const unsigned char icn_empty_bits[] =  { 0xc3,0xe7,0x7e,0x3c,0x3c,0x7e,0xe7,0xc3 };
static const unsigned char icn_on_bits[] =     { 0x3c,0x42,0x99,0xbd,0xbd,0x99,0x42,0x3c };
static const unsigned char icn_off_bits[] =    { 0x3c,0x42,0x81,0x81,0x81,0x81,0x42,0x3c };
static const unsigned char icn_restart_bits[] = { 0x5c,0x62,0x71,0x01,0x41,0x22,0x1c };  // 7x7, setting restarts the core
static const unsigned char icn_val_left_bits[] = { 0x08,0x0c,0x0e,0x0f,0x0e,0x0c,0x08 }; // 4x7, value can step down
static const unsigned char icn_val_right_bits[] = { 0x01,0x03,0x07,0x0f,0x07,0x03,0x01 };// 4x7, value can step up
// volume bar speaker: body (4x8), two sound waves (4x8), or a small X when muted (4x8)
static const unsigned char icn_speaker_bits[]  = { 0x08,0x0c,0x0f,0x0f,0x0f,0x0f,0x0c,0x08 };
static const unsigned char icn_sound_bits[]    = { 0x00,0x04,0x09,0x0a,0x0a,0x09,0x04,0x00 };
static const unsigned char icn_mute_bits[]     = { 0x00,0x00,0x09,0x06,0x06,0x09,0x00,0x00 };

void u8g2_DrawStrT(u8g2_t *u8g2, u8g2_uint_t x, u8g2_uint_t y, const char *s) {
  // get length of string
  int n = 0;
  while(s[n] && s[n] != ';' && s[n] != ',' && s[n] != '|') n++;

  // create a 0 terminated copy in the stack
  char buffer[n+1];
  strncpy(buffer, s, n);
  buffer[n] = '\0';
  
  u8g2_DrawStr(u8g2, x, y, buffer);
}

#define FS_ICON_WIDTH 10
static int fs_scroll_cur = -1;

static int dir_len(sdc_dir_entry_t *d) {
  int len = 0;
  while(d) {
    d = d->next;
    len++;
  }
  return len;
}

static sdc_dir_entry_t *dir_entry(sdc_dir_entry_t *d, int n) {
  while(n && d) {
    d = d->next;
    n--;
  }
  return d;
}

// file selector row width, leaving room for the scroll bar when needed
static int menu_fsel_width(void) {
  return u8g2_GetDisplayWidth(&u8g2) - (dir_len(menu_state->dir) > 4 ? 3 : 0);
}

static void menu_fs_scroll_entry(void) {
  // no scrolling
  if(fs_scroll_cur < 0) return;

  // don't scroll anything else
  if(menu_state->type != MENU_TYPE_FILESELECTOR) return;

  int row = menu_state->selected - 1;
  int y =  MENU_LINE_Y + MENU_ENTRY_H * (row-menu_state->scroll+1);
  int width = menu_fsel_width();

  // jump to the row'th entry in the dir listing
  int swid = u8g2_GetStrWidth(&u8g2, dir_entry(menu_state->dir, row)->name) + 1;

  // fill the area where the scrolling entry would show
  u8g2_SetClipWindow(&u8g2, FS_ICON_WIDTH, y-MENU_ENTRY_BASE, width, y+MENU_ENTRY_H-MENU_ENTRY_BASE);  
  u8g2_DrawBox(&u8g2, FS_ICON_WIDTH, y-MENU_ENTRY_BASE, width-FS_ICON_WIDTH, MENU_ENTRY_H);
  u8g2_SetDrawColor(&u8g2, 0);

  int scroll = fs_scroll_cur++ - 25;   // 25 means 1 sec delay
  if(fs_scroll_cur > swid-width+FS_ICON_WIDTH+50) fs_scroll_cur = 0;
  if(scroll < 0) scroll = 0;
  if(scroll > swid-width+FS_ICON_WIDTH) scroll = swid-width+FS_ICON_WIDTH;
  
  u8g2_DrawStr(&u8g2, FS_ICON_WIDTH-scroll, y, dir_entry(menu_state->dir, row)->name);
  
  // restore previous draw mode
  u8g2_SetDrawColor(&u8g2, 1);
  u8g2_SetMaxClipWindow(&u8g2);
  u8g2_SendBuffer(&u8g2);
}

void menu_timer_enable(bool on);

static void menu_fs_draw_entry(int row, sdc_dir_entry_t *entry) {      
  static const unsigned char folder_icon[] = { 0x70,0x8e,0xff,0x81,0x81,0x81,0x81,0x7e };
  static const unsigned char up_icon[] =     { 0x04,0x0e,0x1f,0x0e,0xfe,0xfe,0xfe,0x00 };
  static const unsigned char empty_icon[] =  { 0xc3,0xe7,0x7e,0x3c,0x3c,0x7e,0xe7,0xc3 };

  int y =  MENU_LINE_Y + MENU_ENTRY_H * (row+1);
  int width = menu_fsel_width();

  if(entry->name[0] == '/') {
    if(fsel_state.none_str) u8g2_DrawStr(&u8g2, FS_ICON_WIDTH, y, fsel_state.none_str);
    else                    u8g2_DrawStr(&u8g2, FS_ICON_WIDTH, y, "No Disk");
  } else {
    char str[strlen(entry->name)+1];
    strcpy(str, entry->name);
  
    // properly ellipsize string
    int dotlen = u8g2_GetStrWidth(&u8g2, "...");
    if(u8g2_GetStrWidth(&u8g2, str) > width-FS_ICON_WIDTH) {
      // the entry is too long to fit the menu.    
      // check if this is the selected file and then enable scrolling
      if(row == menu_state->selected - menu_state->scroll - 1)
	fs_scroll_cur = 0;
      
      // enable timer, to allow animations
      menu_timer_enable(true);
      
      while(u8g2_GetStrWidth(&u8g2, str) > width-FS_ICON_WIDTH-dotlen) str[strlen(str)-1] = 0;
      if(strlen(str) < sizeof(str)-4) strcat(str, "...");
    }
  
    u8g2_DrawStr(&u8g2, FS_ICON_WIDTH, y, str);
  }
  
  // draw folder icon in front of directories
  if(entry->is_dir)
    u8g2_DrawXBM(&u8g2, 1, y-8, 8, 8,
		 (entry->name[0] == '/')?(fsel_state.none_icn?fsel_state.none_icn:empty_icon):
		 strcmp(entry->name, "..")?folder_icon:
		 up_icon);

  if(menu_state->selected == row+menu_state->scroll+1)
    u8g2_DrawButtonFrame(&u8g2, 0, y, U8G2_BTN_INV, width, 1, 1);
}

static void menu_push(void) {
  menu_debugf("menu_push()");

  // allocate a new menu entry
  menu_state_t *state = pvPortMalloc(sizeof(menu_state_t));

  // insert the new entry at begin of chain
  state->prev = menu_state;
  menu_state = state;
}

static int menu_len(const config_menu_t *menu) {
  int entries = 0;
  for(config_menu_entry_t *me = menu->entries; me; me = me->next)
    if(menu_entry_visible(me)) entries++;
  return entries;
}

static int menu_count_entries(void) {
  int entries = 0;

  if(menu_state->type == MENU_TYPE_MENU)
    entries = menu_len(menu_state->menu);
  else if(menu_state->type == MENU_TYPE_FILESELECTOR)
    entries = dir_len(menu_state->dir);
  else if(menu_state->type == MENU_TYPE_CUSTOM)
    if(menu_state->custom->length)
      entries = menu_state->custom->length();
    
  return entries+1;  // title is also an entry
}

static bool menu_is_root(const config_menu_t *menu) {
  return menu == cfg->menu;
}

// The title row works as "back". In the main menu it would close the menu,
// so there it's only used in single button mode (no keyboard or gamepad).
static int menu_title_is_usable(void) {
  bool root = menu_state->type == MENU_TYPE_MENU && menu_is_root(menu_state->menu);
  return !root || !mcu_hw_hid_present();
}

static config_menu_entry_t *menu_get_selected_entry(void) {
  // first check if there's a menu at all
  if(menu_state->type != MENU_TYPE_MENU || menu_state->selected == 0)
    return NULL;

  return menu_entry_nth(menu_state->menu, menu_state->selected - 1);
}
  
static void menu_entry_go(int step) {

  // the up/down events change the a range value in edit mode
  config_menu_entry_t *entry = menu_get_selected_entry();
  if(entry && entry->type == CONFIG_MENU_ENTRY_RANGE && entry->range->edit) {
    int value = menu_variable_get(entry->range->id);

    value -= step;
    if(value > entry->range->max) value = entry->range->max;
    if(value < entry->range->min) value = entry->range->min;          

    menu_variable_set(entry->range->id, value);
    return;
  }							  
  
  int entries = menu_count_entries();   // incl. the title row
  int first = menu_title_is_usable() ? 0 : 1;
  if(entries - 1 < first) return;       // nothing to move to, e.g. About

  int sel = menu_state->selected + step;
  if(!mcu_hw_hid_present() && abs(step) == 1) {
    // single button mode can only step down, so it wraps around
    if(sel >= entries) sel = first;
    if(sel < first)    sel = entries - 1;
  } else {
    // stop at the top and bottom
    if(sel < first)    sel = first;
    if(sel >= entries) sel = entries - 1;
  }
  menu_state->selected = sel;

  // scrolling needed?
  if(step > 0) {
    if(entries <= 5)                            menu_state->scroll = 0;
    else {
      if(menu_state->selected <= 3)             menu_state->scroll = 0;
      else if(menu_state->selected < entries-2) menu_state->scroll = menu_state->selected - 3;
      else                                      menu_state->scroll = entries-5;
    }
  }

  if(step < 0) {
    if(entries <= 5)                            menu_state->scroll = 0;
    else {
      if(menu_state->selected <= 2)             menu_state->scroll = 0;
      else if(menu_state->selected < entries-3) menu_state->scroll = menu_state->selected - 2;
      else                                      menu_state->scroll = entries-5;
    }
  }
}

static void menu_draw_title(const char *s, bool arrow, bool selected) {
  int x = 1;

  // draw left arrow for submenus
  if(arrow) {
    u8g2_DrawXBM(&u8g2, 0, 1, 8, 8, icn_left_bits);    
    x = 8;
  } else if(!mcu_hw_hid_present()) {
    // without keyboard or joystick connected, the menu runs in
    // "single button" mode
  
    // no arrow? Then this is the root menu. In that case
    // a cross is drawn in "single button mode"
    u8g2_DrawXBM(&u8g2, 0, 1, 8, 8, icn_empty_bits);    
    x = 10;
  }
  
  // draw title in bold and seperator line
  u8g2_SetFont(&u8g2, u8g2_font_helvB08_tr);
  u8g2_DrawStr(&u8g2, x, MENU_ENTRY_BASE, s);
  u8g2_DrawHLine(&u8g2, 0, MENU_LINE_Y, u8g2_GetDisplayWidth(&u8g2));

  if(selected)
    u8g2_DrawButtonFrame(&u8g2, 0, MENU_ENTRY_BASE, U8G2_BTN_INV,
	 u8g2_GetDisplayWidth(&u8g2), 1, 1);
  
  // draw the rest with normal font
  u8g2_SetFont(&u8g2, font_helvR08_te);
}

static char *menuentry_get_label(config_menu_entry_t *entry) {
  if(entry->type == CONFIG_MENU_ENTRY_MENU)
    return entry->menu->label;
  if(entry->type == CONFIG_MENU_ENTRY_FILESELECTOR)
    return entry->fsel->label;
  if(entry->type == CONFIG_MENU_ENTRY_LIST)
    return entry->list->label;
  if(entry->type == CONFIG_MENU_ENTRY_BUTTON)
    return entry->button->label;
  if(entry->type == CONFIG_MENU_ENTRY_IMAGE)
    return entry->image->label;
  if(entry->type == CONFIG_MENU_ENTRY_TOGGLE)
    return entry->toggle->label;
  if(entry->type == CONFIG_MENU_ENTRY_RANGE)
    return entry->range->label;
  if(entry->type == CONFIG_MENU_ENTRY_OPTION)
    return entry->option->label;
  if(entry->type == CONFIG_MENU_ENTRY_INFO)
    return entry->info->label;

  return NULL;
}

// index of an option's current value in its choices, -1 if it's none of them
static int menu_option_index(const config_option_t *o) {
  int value = inifile_option_get(o->option);
  for(int i=0;i<o->count;i++)
    if((o->values ? o->values[i] : i) == value) return i;
  return -1;
}

// list value shown and changed in the menu: a waiting (pending) one if there is
static int menu_list_value(const config_list_t *list);
  
static char *menu_get_listentry(config_menu_entry_t *entry, int value) {
  if(!entry || entry->type != CONFIG_MENU_ENTRY_LIST) return NULL;

  config_listentry_t *le = entry->list->listentries;
  while(le) {
    if(le->value == value)
      return le->label;
    le = le->next;
  }

  return NULL;
}

// copy s into buf, shortened with "..." to fit maxw pixels
static void menu_fit_str(char *buf, int size, const char *s, int maxw) {
  snprintf(buf, size, "%s", s);
  if(u8g2_GetStrWidth(&u8g2, buf) <= maxw) return;

  int dotlen = u8g2_GetStrWidth(&u8g2, "...");
  int n = strlen(buf);
  while(n > 0 && u8g2_GetStrWidth(&u8g2, buf) > maxw - dotlen) buf[--n] = 0;
  if(n + 4 <= size) strcat(buf, "...");
}

// width is the row width, smaller than the display when a scroll bar is shown
static void menu_draw_entry(config_menu_entry_t *entry, int row, bool selected, int width) {
  menu_debugf("row %d: %s '%s'", row,
	      config_menuentry_get_type_str(entry),
	      menuentry_get_label(entry));

  // all menu entries use some kind of label
  char *s = menuentry_get_label(entry);
  int ypos = MENU_LINE_Y+MENU_ENTRY_H + MENU_ENTRY_H * row;

  u8g2_DrawStr(&u8g2, 1, ypos, s);
  int lend = 1 + u8g2_GetStrWidth(&u8g2, s);

  // mark settings that run an action when changed, e.g. restart the core
  if(entry->type == CONFIG_MENU_ENTRY_LIST && entry->list->action) {
    u8g2_DrawXBM(&u8g2, lend+3, ypos-8, 7, 7, icn_restart_bits);
    lend += 10;
  }

  // the value shown on the right; settings that Left/Right can change get arrows
  const char *value = NULL;
  bool adjustable = false;
  char num[8];

  switch(entry->type) {
  case CONFIG_MENU_ENTRY_LIST:
    value = menu_get_listentry(entry, menu_list_value(entry->list));
    adjustable = true;
    break;

  case CONFIG_MENU_ENTRY_OPTION: {
    int i = menu_option_index(entry->option);
    if(i >= 0) value = entry->option->labels[i];
    else { snprintf(num, sizeof(num), "%d", inifile_option_get(entry->option->option)); value = num; }
    adjustable = true;
  } break;

  case CONFIG_MENU_ENTRY_RANGE:
    snprintf(num, sizeof(num), "%d", menu_variable_get(entry->range->id));
    value = num;
    adjustable = true;
    break;

  case CONFIG_MENU_ENTRY_INFO:
    value = entry->info->text();
    break;

  case CONFIG_MENU_ENTRY_FILESELECTOR:
    value = sdc_get_image_name(entry->fsel->index);
    if(!value) value = "-";
    break;

  case CONFIG_MENU_ENTRY_IMAGE:
    value = sdc_get_image_name(entry->image->index+MAX_DRIVES);
    if(!value) value = entry->image->none_str ? entry->image->none_str : "-";
    break;

  case CONFIG_MENU_ENTRY_MENU:
    u8g2_DrawXBM(&u8g2, width-8, ypos-8, 8, 8, icn_right_bits);
    break;

  case CONFIG_MENU_ENTRY_TOGGLE:
    u8g2_DrawXBM(&u8g2, width-9, ypos-8, 8, 8,
		 menu_variable_get(entry->toggle->id)?icn_on_bits:icn_off_bits);
    break;
  }

  if(value) {
    bool arrows = selected && adjustable;
    char str[48];
    menu_fit_str(str, sizeof(str), value, width - lend - 5 - (arrows ? 12 : 0));
    int vx = width - 1 - u8g2_GetStrWidth(&u8g2, str) - (arrows ? 6 : 0);
    u8g2_DrawStr(&u8g2, vx, ypos, str);
    if(arrows) {
      u8g2_DrawXBM(&u8g2, vx-6, ypos-8, 4, 7, icn_val_left_bits);
      u8g2_DrawXBM(&u8g2, width-5, ypos-8, 4, 7, icn_val_right_bits);
    }
  }

  if(selected)
    u8g2_DrawButtonFrame(&u8g2, 0, ypos, U8G2_BTN_INV, width, 1, 1);
}

// scroll bar on the right when a page has more than four rows
static void menu_draw_scrollbar(int entries, int scroll) {
  if(entries <= 4) return;
  int top = MENU_LINE_Y + 1, track = 64 - top;
  for(int y = top + 1; y < 64; y += 2) u8g2_DrawPixel(&u8g2, 127, y);
  int th = track * 4 / entries;
  if(th < 6) th = 6;
  u8g2_DrawBox(&u8g2, 126, top + (track - th) * scroll / (entries - 4), 2, th);
}

// Draws msg centered in lines no wider than the display, from y_in down, and
// returns the y below the last line. With y_in 0 it only measures. A '\n' ends
// a line, the rest breaks at spaces, and a word wider than the display is split.
static int menu_wrap_text(int y_in, const char *msg) {
  const int width = u8g2_GetDisplayWidth(&u8g2);
  char line[80];
  int y = y_in;

  u8g2_SetFont(&u8g2, font_helvR08_te);
  if(!*msg) return y + 11;   // an empty message still takes one line

  while(*msg) {
    size_t take = 0;   // characters that fit for sure
    size_t gap  = 0;   // last space among them, 0 = none
    size_t i;

    while(*msg == ' ') msg++;                        // a line never starts with a space
    if(*msg == '\n') { y += 11; msg++; continue; }   // an empty line
    if(!*msg) break;

    // take characters until the line is too wide, the buffer is full or the text ends
    for(i = 0; msg[i] && msg[i] != '\n' && i < sizeof(line) - 1; i++) {
      line[i]     = msg[i];
      line[i + 1] = '\0';
      if(u8g2_GetStrWidth(&u8g2, line) > width) break;
      if(msg[i] == ' ') gap = i;
      take = i + 1;
    }

    // stopped inside a word with a space before it: break at the space instead.
    // Stopped at a space: everything before it fits, the line ends right there.
    if(msg[i] && msg[i] != '\n' && msg[i] != ' ' && gap) take = gap;
    if(take == 0) take = 1;   // even one character is too wide, it goes out anyway
    while(take > 1 && msg[take - 1] == ' ') take--;   // spaces at the end would shift the centering

    line[take] = '\0';
    if(y_in) u8g2_DrawStr(&u8g2, (width - u8g2_GetStrWidth(&u8g2, line)) / 2, y, line);
    y += 11;

    // the next line starts after the '\n' that ended this one
    msg += take;
    while(*msg == ' ') msg++;
    if(*msg == '\n') msg++;
  }

  return y;
}

// draw a dialog box
static bool dialog_opened_osd = false;
static TimerHandle_t dialog_disappear_timer = NULL;  
static char network_ipaddr[16];
static bool network_was_connected = false;

static bool menu_dialog_is_open(void) {
  if(!dialog_disappear_timer) return false;  // no timer -> no dialog
  return xTimerIsTimerActive(dialog_disappear_timer) != pdFALSE;
}

static void menu_dialog_timeout(__attribute__((unused)) TimerHandle_t arg) {
  menu_debugf("Close dialog");
  if(dialog_opened_osd) osd_enable(OSD_INVISIBLE);    
  else                  menu_do(0);
}

static void menu_dialog_close(void) {
  if(!dialog_disappear_timer) return;
  xTimerStop(dialog_disappear_timer, 0);
  menu_dialog_timeout(NULL);
}
  
static void menu_draw_dialog_for(const char *title, const char *msg,
                                  TickType_t duration) {
  // check if osd is already visible
  if(!menu_dialog_is_open()) {
    dialog_opened_osd = !osd_is_visible();
    if(dialog_opened_osd) osd_enable(OSD_VISIBLE);
  }

  if(!dialog_disappear_timer)
    dialog_disappear_timer = xTimerCreate("Dialog timer", duration, pdFALSE,
					   NULL, menu_dialog_timeout);
  else
    xTimerChangePeriod(dialog_disappear_timer, duration, 0);
  xTimerStart(dialog_disappear_timer, 0);
  
  u8g2_ClearBuffer(&u8g2);

  // MENU_LINE_Y is the height of the title incl line
  int y = (64 - MENU_LINE_Y - menu_wrap_text(0, msg))/2;
  
  u8g2_SetFont(&u8g2, u8g2_font_helvB08_tr);
  
  int width = u8g2_GetDisplayWidth(&u8g2);
  int swid = u8g2_GetStrWidth(&u8g2, title);
 
  // draw title in bold and seperator line
  u8g2_DrawStr(&u8g2, (width-swid)/2, y+MENU_ENTRY_BASE, title);
  u8g2_DrawHLine(&u8g2, (width-swid)/2, y+MENU_ENTRY_H, swid);

  u8g2_SetFont(&u8g2, font_helvR08_te);

  menu_wrap_text(y+23, msg);
  
  u8g2_SendBuffer(&u8g2);
}

void menu_draw_dialog(const char *title, const char *msg) {
  menu_draw_dialog_for(title, msg, pdMS_TO_TICKS(2000));
}

static void menu_draw(const config_menu_t *menu, int selected, int scroll) {
  u8g2_ClearBuffer(&u8g2);
 
  // =============== draw a regular menu =================
  menu_debugf("drawing '%s'", menu->label);  
    
  // draw the title
  menu_draw_title(menu->label, !menu_is_root(menu), selected == 0);

  // draw up to four visible entries from "scroll" on
  int entries = menu_len(menu);
  int width = u8g2_GetDisplayWidth(&u8g2) - (entries > 4 ? 3 : 0);
  for(int i=0;i<4;i++) {
    config_menu_entry_t *entry = menu_entry_nth(menu, scroll+i);
    if(!entry) break;
    menu_draw_entry(entry, i, selected == scroll+i+1, width);
  }
  menu_draw_scrollbar(entries, scroll);

  u8g2_SendBuffer(&u8g2);
}

static void menu_fsel_draw(const char *label, sdc_dir_entry_t *dir, int selected, int scroll) {
  u8g2_ClearBuffer(&u8g2);

  // =============== draw a fileselector =================    
  menu_debugf("drawing '%s'", label);
  
  menu_draw_title(label, true, selected == 0);
  menu_timer_enable(false);
  fs_scroll_cur = -1;

  // draw up to four entries
  for(int i=0;i<4 && i<dir_len(dir)-scroll;i++) {
    menu_debugf("file %s", dir_entry(dir, i+scroll)->name);
    menu_fs_draw_entry(i, dir_entry(dir, i+scroll));
  }
  menu_draw_scrollbar(dir_len(dir), scroll);
  u8g2_SendBuffer(&u8g2);
}

static void menu_custom_draw(const config_custom_t *custom, int selected, __attribute__((unused)) int scroll) {
  u8g2_ClearBuffer(&u8g2);

  // =============== draw a custom dialog =================    
  menu_debugf("drawing custom '%s' (%d)", custom->label, selected);

  // title is a regular entry
  menu_draw_title(custom->label, true, selected == 0);

  if(custom->draw)
    custom->draw();
  
  u8g2_SendBuffer(&u8g2);
}

void menu_goto(const config_menu_t *menu) {
  menu_push();
  
  // prepare menu state ...
  menu_state->menu = menu;
  menu_state->selected = 1;
  menu_state->scroll = 0;
  menu_state->type = MENU_TYPE_MENU;
}

static void menu_file_selector_open(config_menu_entry_t *entry) {
  menu_debugf("menu_file_selector_open()");

  // the file selector can either be opened from disk image file
  // selectors or from rom image selectors  
  if(entry->type == CONFIG_MENU_ENTRY_IMAGE) {
    menu_debugf("opening rom image selector");
    fsel_state.ext = entry->image->ext;
    fsel_state.index = entry->image->index + MAX_DRIVES;  // the images are stored after the drives
    fsel_state.none_str = entry->image->none_str;
    fsel_state.none_icn = entry->image->none_icn;
    fsel_state.action = entry->image->action;
  } else {
    menu_debugf("opening drive image selector");
    fsel_state.ext = entry->fsel->ext;
    fsel_state.index = entry->fsel->index;
    fsel_state.none_str = NULL;
    fsel_state.none_icn = NULL;
    fsel_state.action = entry->fsel->action;
  }
    
  // The file selector usually works on the sd card as that is what
  // the cores are reading data from. But cores may be loaded from
  // USB as well. The file selectors default path will begin with
  // "/usb" in that case

  // Initialize current working directory if needed
  if(!sdc_get_cwd(fsel_state.index)) {
    bool is_usb = strncasecmp(entry->fsel->def, "/usb", 4) == 0;
    
    // check if USB is to be used but isn't present
    if(is_usb && (disk_status(1) != RES_OK)) {
      menu_draw_dialog("USB Error", "No USB mass storage device connected!");
      return;
    }

    sdc_set_cwd(fsel_state.index, is_usb?"/usb":"/sd");
  }

  // IMAGE and file selectors are both drawn by the fileselector routines
  menu_push();
  menu_state->label = (entry->type == CONFIG_MENU_ENTRY_IMAGE)?entry->image->label:entry->fsel->label;
  menu_state->selected = 1;
  menu_state->scroll = 0;
  menu_state->type = MENU_TYPE_FILESELECTOR;
  
  // scan file system
  menu_state->dir = sdc_readdir(fsel_state.index, NULL, (void*)fsel_state.ext);
  // try to jump to current file. Get the current image name and path
  char *name = sdc_get_image_name(fsel_state.index);
  if(name) {
    debugf("trying to jump to %s", name);
    // try to find name in file list
    for(int i=0;i<dir_len(menu_state->dir);i++) {
      if(strcmp(dir_entry(menu_state->dir, i)->name, name) == 0) {
	debugf("found preset entry %d", i);
	
	// file found, adjust entry and offset
	menu_state->selected = i+1;
	
	if(dir_len(menu_state->dir) > 4 && menu_state->selected > 3) {
	  debugf("more than 4 files and selected is > 3");
	  if(menu_state->selected < dir_len(menu_state->dir)-1) menu_state->scroll = menu_state->selected - 3;
	  else                                                  menu_state->scroll = dir_len(menu_state->dir)-4;
	}
      }
    }
  }  
}

// all other entries in step down
static void menu_pop(void) {
  menu_debugf("menu_pop()");

  // "pop"ing the root menu means to hide it. This will only
  // happen in single button mode with the roots title
  if(!menu_state->prev) {
    osd_enable(OSD_INVISIBLE);
    return;
  }

  // neither should this as we never really close the
  // root menu
  if(menu_state->menu == cfg->menu) {
    vPortFree(menu_state);
    menu_state = NULL;
    return;
  }

  // de-chain first entry
  menu_state_t *m = menu_state;
  menu_state = menu_state->prev;
  vPortFree(m);
}

static void menu_fileselector_select(sdc_dir_entry_t *entry) {
  int drive = fsel_state.index;
  menu_debugf("%s %d: file selected '%s'", drive>=MAX_DRIVES?"IMG":"DRV",
	      drive>=MAX_DRIVES?drive-MAX_DRIVES:drive, entry->name);
    
  // stop any scroll timer that might be running
  menu_timer_enable(false);
    
  if(entry->is_dir) {
    if(entry->name[0] == '/') {
      // User selected the "No Disk" entry
      // return to parent form
      menu_pop();
      // Eject
      sdc_image_open(drive, NULL);
      menu_dirty = true;
    } else {	
      // check if we are going up one dir and try to select the
      // directory we are coming from
      char *prev = NULL; 
      if(strcmp(entry->name, "..") == 0) {
	prev = strrchr(sdc_get_cwd(drive), '/');
	if(prev) prev++;
      }

      menu_state->selected = 1;   // start by highlighting '..'
      menu_state->scroll = 0;
      menu_state->dir = sdc_readdir(drive, entry->name, (void*)fsel_state.ext);	
      
      // prev is still valid, since sdc_readdir doesn't free the old string when going
      // up one directory. Instead it just terminates it in the middle	
      if(prev) {
	menu_debugf("up to %s", prev);
	
	// try to find previous dir entry in current dir	  
	for(int i=0;i<dir_len(menu_state->dir);i++) {
	  if(dir_entry(menu_state->dir, i)->is_dir && strcmp(dir_entry(menu_state->dir, i)->name, prev) == 0) {
	    // file found, adjust entry and offset
	    menu_state->selected = i+1;

	    if(dir_len(menu_state->dir) > 4 && menu_state->selected > 3) {
	      if(menu_state->selected < dir_len(menu_state->dir) - 1) menu_state->scroll = menu_state->selected - 3;
	      else                                                    menu_state->scroll = menu_state->selected - 5;
	    }
	  }
	}
      }
    }
  } else {
    // request insertion of this image
    sdc_image_open(drive, entry->name);
    menu_dirty = true;

    // return to parent form
    menu_pop();

    // check if this is a drive image selection and run action if yes. Image selectors
    // work differently and do an IRQ driven transfer in the background. The action is there executed
    // ocne the transfer itself is done.
    if(drive<MAX_DRIVES && fsel_state.action)
      sys_run_action(fsel_state.action);
  }
}

void menu_run_current_image_action(void) {
  // Note: The download may also have been triggered when loading config from
  // ini file. We may need to find the right action in that case as well.
  // The same is true for mounted disk images
  
  if(!menu_state) return;

  // the current menu state will be a open file selector if the image transfer
  // has been started through the OSD by the user
  if(menu_state->type != MENU_TYPE_FILESELECTOR) {
    menu_debugf("finished download was ini file triggered during boot");

    if(!sdc_check_for_pending_image_uploads()) {
      menu_debugf("no more image uploads pending");

      sys_run_action_by_name("ready");
    }
      
    return;
  }

  // fsel index should be >= MAX_DRIVES and < MAX_DRIVES+MAX_IMAGES as it
  // should point to an image selector
  if(fsel_state.index < MAX_DRIVES || fsel_state.index >= MAX_DRIVES+MAX_IMAGES) return;

  // run the file selector action if one is present
  if(fsel_state.action)
    sys_run_action(fsel_state.action);

}

/* ============ changing values, waiting changes and saving ============ */

static int menu_list_value(const config_list_t *list) {
  return (list->pending >= 0) ? list->pending : menu_variable_get(list->id);
}

// step a list to the previous (-1) or next (1) entry, in list order. Select
// wraps around at the ends, Left/Right stop there. Lists with an action
// (e.g. a restart) only take effect once the page is left, see menu_leave().
static void menu_list_step(config_menu_entry_t *entry, int dir, bool wrap) {
  config_list_t *list = entry->list;
  int value = menu_list_value(list);

  config_listentry_t *cur = list->listentries, *prev = NULL, *last = NULL;
  for(config_listentry_t *le = list->listentries; le; le = le->next) {
    if(le->value == value) { cur = le; break; }
    prev = le;
  }
  if(!cur) return;
  if(dir < 0 && !prev) for(prev = cur; prev; prev = prev->next) last = prev;  // last entry

  config_listentry_t *next = (dir > 0) ? (cur->next ? cur->next : (wrap ? list->listentries : NULL))
                                       : (prev ? prev : (wrap ? last : NULL));
  if(!next || next == cur) return;

  if(list->action)
    list->pending = (next->value == menu_variable_get(list->id)) ? -1 : next->value;
  else {
    menu_variable_set(list->id, next->value);
    menu_dirty = true;
  }
}

static void menu_option_step(config_option_t *o, int dir, bool wrap) {
  int i = menu_option_index(o) + dir;
  if(i < 0)         i = wrap ? o->count-1 : 0;
  if(i >= o->count) i = wrap ? 0 : o->count-1;
  inifile_option_set(o->option, o->values ? o->values[i] : i);
  menu_dirty = true;
}

static void menu_range_step(config_range_t *r, int dir) {
  int value = menu_variable_get(r->id) + dir;
  if(value > r->max) value = r->max;
  if(value < r->min) value = r->min;
  menu_variable_set(r->id, value);
  menu_dirty = true;
}

// does the current page have changes waiting to be applied?
static bool menu_page_pending(void) {
  if(menu_state->type != MENU_TYPE_MENU) return false;
  for(config_menu_entry_t *e = menu_state->menu->entries; e; e = e->next)
    if(e->type == CONFIG_MENU_ENTRY_LIST && e->list->pending >= 0) return true;
  return false;
}

// the core's settings file, from its "save" action
static char *menu_save_file(void) {
  for(config_action_t *a = cfg->actions; a; a = a->next)
    for(config_action_command_t *c = a->commands; c; c = c->next)
      if(c->code == CONFIG_ACTION_COMMAND_SAVE) return c->filename;
  return NULL;
}

// save changed settings without a message, like the core's Save button but silent
static void menu_autosave(void) {
  if(!menu_dirty) return;
  char *file = menu_save_file();
  if(!file) return;
  if(inifile_write(file) == 0) menu_dirty = false;
  else menu_draw_dialog("Settings", "save failed");
}

static void menu_close(void) {
  menu_timer_enable(false);
  menu_autosave();
  osd_enable(OSD_INVISIBLE);
}

// apply or drop the waiting changes of a page. Applying sets them all, saves,
// then runs each of their actions once, e.g. one restart for several settings.
static void menu_apply_pending(const config_menu_t *menu, bool apply) {
  config_action_t *actions[8];
  int n = 0;

  for(config_menu_entry_t *e = menu->entries; e; e = e->next) {
    if(e->type != CONFIG_MENU_ENTRY_LIST || e->list->pending < 0) continue;
    if(apply) {
      menu_variable_set(e->list->id, e->list->pending);
      menu_dirty = true;
      int i;
      for(i=0;i<n && actions[i] != e->list->action;i++);
      if(i == n && n < 8) actions[n++] = e->list->action;
    }
    e->list->pending = -1;
  }

  if(!apply) return;
  menu_autosave();
  for(int i=0;i<n;i++) sys_run_action(actions[i]);
}

static void menu_confirm_choose(bool apply) {
  const config_menu_t *page = menu_state->menu;
  bool close = confirm.close;
  confirm.open = false;

  // leave the page first, the actions may hide the OSD
  if(!close) menu_pop();
  menu_apply_pending(page, apply);
  if(close && osd_is_visible()) menu_close();
}

static void menu_confirm_draw(void) {
  u8g2_ClearBuffer(&u8g2);
  u8g2_DrawFrame(&u8g2, 3, 3, 122, 58);
  u8g2_SetFont(&u8g2, u8g2_font_helvB08_tr);
  const char *title = "Restart needed";
  u8g2_DrawStr(&u8g2, (128 - u8g2_GetStrWidth(&u8g2, title))/2, 17, title);
  u8g2_SetFont(&u8g2, font_helvR08_te);
  const char *msg = "Apply and restart?";
  u8g2_DrawStr(&u8g2, (128 - u8g2_GetStrWidth(&u8g2, msg))/2, 31, msg);

  const char *btn[2] = { "Apply", "Discard" };
  for(int i=0;i<2;i++) {
    int bx = 12 + i*56;
    u8g2_DrawFrame(&u8g2, bx, 40, 48, 14);
    u8g2_DrawStr(&u8g2, bx + (48 - u8g2_GetStrWidth(&u8g2, btn[i]))/2, 51, btn[i]);
    if(i == confirm.sel) {
      u8g2_SetDrawColor(&u8g2, 2);
      u8g2_DrawBox(&u8g2, bx, 40, 48, 14);
      u8g2_SetDrawColor(&u8g2, 1);
    }
  }
  u8g2_SendBuffer(&u8g2);
}

/* ============ core error message ============ */

// Shown when the core reports an error number, e.g. a missing drive ROM.
// The text comes from the core's config (<error code=".." .../>). It stays
// until OK is pressed.
static struct {
  bool open;
  bool opened_osd;    // the message opened the OSD, closing it hides it again
  char title[32];
  char text[100];     // up to three lines, separated by '|'
} core_err;

static const unsigned char icn_warning_bits[] = { 0x08,0x14,0x1c,0x2a,0x22,0x49,0x7f };  // 7x7

static void menu_core_error_draw(void) {
  u8g2_ClearBuffer(&u8g2);

  // title bar: inverted, with a warning sign
  u8g2_DrawBox(&u8g2, 0, 0, 128, 12);
  u8g2_SetDrawColor(&u8g2, 0);
  u8g2_DrawXBM(&u8g2, 3, 2, 7, 7, icn_warning_bits);
  u8g2_SetFont(&u8g2, u8g2_font_helvB08_tr);
  u8g2_DrawStr(&u8g2, 13, 9, core_err.title);
  u8g2_SetDrawColor(&u8g2, 1);

  // up to three centered lines
  u8g2_SetFont(&u8g2, font_helvR08_te);
  const char *s = core_err.text;
  for(int i=0;i<3 && *s;i++) {
    char line[48];
    int n = 0;
    while(s[n] && s[n] != '|' && n < (int)sizeof(line)-1) { line[n] = s[n]; n++; }
    line[n] = '\0';
    u8g2_DrawStr(&u8g2, (128 - u8g2_GetStrWidth(&u8g2, line)) / 2, 23 + 11*i, line);
    s += n;
    if(*s == '|') s++;
  }

  // OK button, always selected
  u8g2_DrawBox(&u8g2, 48, 51, 32, 13);
  u8g2_SetDrawColor(&u8g2, 0);
  u8g2_DrawStr(&u8g2, 64 - u8g2_GetStrWidth(&u8g2, "OK") / 2, 61, "OK");
  u8g2_SetDrawColor(&u8g2, 1);

  u8g2_SendBuffer(&u8g2);
}

static void menu_core_error(int code) {
  const config_error_t *e = config_get_error(code);
  snprintf(core_err.title, sizeof(core_err.title), "%s", (e && e->title) ? e->title : "Core error");
  if(e && e->text) snprintf(core_err.text, sizeof(core_err.text), "%s", e->text);
  else             snprintf(core_err.text, sizeof(core_err.text), "The core reports|error %d.", code);

  if(!core_err.open) core_err.opened_osd = !osd_is_visible();
  core_err.open = true;
  if(!osd_is_visible()) osd_enable(OSD_VISIBLE);
  menu_core_error_draw();
}

static void menu_core_error_close(void) {
  core_err.open = false;
  if(core_err.opened_osd) osd_enable(OSD_INVISIBLE);
}

// close the menu, asking first if the page has waiting changes
static void menu_leave_all(void) {
  if(menu_page_pending()) {
    confirm.open = true; confirm.sel = 0; confirm.close = true;
    return;
  }
  menu_close();
}

// user has pressed esc to go back one level
static void menu_back(void) {
  // when in range edit mode, back leaves edit mode
  config_menu_entry_t *entry = menu_get_selected_entry();
  if(entry && entry->type == CONFIG_MENU_ENTRY_RANGE && entry->range->edit) {
    entry->range->edit = false;
    return;
  }							  

  // stop doing the scroll timer
  menu_timer_enable(false);

  // leaving a page with waiting changes asks first
  if(menu_page_pending()) {
    confirm.open = true; confirm.sel = 0;
    confirm.close = menu_is_root(menu_state->menu);
    return;
  }

  // are we in the root menu?
  if(menu_state->type == MENU_TYPE_MENU && menu_is_root(menu_state->menu))
    menu_close();
  else {
    // are we in fileselector?
    if(menu_state->type == MENU_TYPE_FILESELECTOR) {
      // search for ".." in current dir
      sdc_dir_entry_t *entry = NULL;
      for(int i=0;i<dir_len(menu_state->dir);i++)
	if(!strcmp(dir_entry(menu_state->dir, i)->name, ".."))
	  entry = dir_entry(menu_state->dir, i);
      
      // if there was one, go up. Else quit the file selector
      if(entry) menu_fileselector_select(entry);
      else      menu_pop();
    } else
      menu_pop();
  }
}

// user has selected a menu entry
static void menu_select(void) {
  // when in range edit mode, select leaves edit mode
  config_menu_entry_t *entry = menu_get_selected_entry();
  if(entry && entry->type == CONFIG_MENU_ENTRY_RANGE && entry->range->edit) {
    entry->range->edit = false;
    return;
  }							  

  // if the title was selected (single button mode), then go back
  if(menu_state->selected == 0) {
    menu_back();
    return;
  }

  // in fileselector
  if(menu_state->type == MENU_TYPE_FILESELECTOR) {
    menu_fileselector_select(dir_entry(menu_state->dir, menu_state->selected-1));
    return;
  }

  // custom pages (e.g. About) have nothing to select
  if(!entry) return;

  menu_debugf("Selected: %s '%s'", config_menuentry_get_type_str(entry), menuentry_get_label(entry));

  switch(entry->type) {
  case CONFIG_MENU_ENTRY_FILESELECTOR:
    // user has choosen a file selector
    menu_file_selector_open(entry);
    break;
    
  case CONFIG_MENU_ENTRY_MENU:
    menu_goto(entry->menu);
    break;

  case CONFIG_MENU_ENTRY_LIST:
    // go to the next entry in list order and back to the first one after
    // the last. The values need not be 0..n-1, e.g. DIP switch settings
    // like 3, 2, 1. A list's action (e.g. a cold boot) runs once the
    // page is left and the change confirmed.
    menu_list_step(entry, 1, true);
    break;

  case CONFIG_MENU_ENTRY_OPTION:
    menu_option_step(entry->option, 1, true);
    break;

  case CONFIG_MENU_ENTRY_BUTTON:
    // buttons often reset the core and hide the menu: save changes first
    menu_autosave();
    if(entry->button->action)
      sys_run_action(entry->button->action);
    break;
	
  case CONFIG_MENU_ENTRY_IMAGE:
    // user has choosen an image selector
    menu_file_selector_open(entry);
    break;
    
  case CONFIG_MENU_ENTRY_TOGGLE:
    menu_variable_set(entry->toggle->id, !menu_variable_get(entry->toggle->id));
    menu_dirty = true;
    if(entry->toggle->action)
      sys_run_action(entry->toggle->action);
    break;

  case CONFIG_MENU_ENTRY_RANGE:
    // selecting a range entry enables changing its value with up/down
    entry->range->edit = true;
    break;

  case CONFIG_MENU_ENTRY_INFO:
    break;

  default:
    menu_debugf("unknown %s", config_menuentry_get_type_str(entry));
  }
}

// Left/Right: change the value of the selected row. Left on a row without
// a value goes back, Right on a submenu or file selector opens it.
static void menu_left_right(int dir) {
  if(menu_state->type == MENU_TYPE_FILESELECTOR) {
    if(dir < 0) menu_back();
    else if(menu_state->selected > 0 && dir_entry(menu_state->dir, menu_state->selected-1)->is_dir)
      menu_select();
    return;
  }

  config_menu_entry_t *entry = menu_get_selected_entry();
  if(entry) {
    switch(entry->type) {
    case CONFIG_MENU_ENTRY_LIST:   menu_list_step(entry, dir, false);         return;
    case CONFIG_MENU_ENTRY_OPTION: menu_option_step(entry->option, dir, false); return;
    case CONFIG_MENU_ENTRY_RANGE:  menu_range_step(entry->range, dir);       return;

    case CONFIG_MENU_ENTRY_TOGGLE:
      if((dir > 0) != (menu_variable_get(entry->toggle->id) != 0)) menu_select();
      return;

    case CONFIG_MENU_ENTRY_MENU:
    case CONFIG_MENU_ENTRY_FILESELECTOR:
    case CONFIG_MENU_ENTRY_IMAGE:
      if(dir > 0) { menu_select(); return; }
      break;
    }
  }

  if(dir < 0) menu_back();
}

// timer implementing key repeat
static TimerHandle_t menu_key_repeat_timer = NULL;  
static int menu_key_last_event = -1;

// draw the current page, or the "Apply and restart?" question over it
static void menu_redraw(void) {
  if(menu_dialog_is_open()) return;
  if(core_err.open) { menu_core_error_draw(); return; }
  if(confirm.open) { menu_confirm_draw(); return; }

  if(menu_state->type == MENU_TYPE_MENU)
    menu_draw(menu_state->menu, menu_state->selected, menu_state->scroll);
  else if(menu_state->type == MENU_TYPE_FILESELECTOR)
    menu_fsel_draw(menu_state->label, menu_state->dir, menu_state->selected, menu_state->scroll);
  else if(menu_state->type == MENU_TYPE_CUSTOM)
    menu_custom_draw(menu_state->custom, menu_state->selected, menu_state->scroll);
}

static void menu_key_repeat(__attribute__((unused)) TimerHandle_t arg) {
  if(menu_key_last_event >= 0 && !confirm.open && !core_err.open) {

    if(menu_key_last_event == MENU_EVENT_UP)     menu_entry_go(-1);
    if(menu_key_last_event == MENU_EVENT_DOWN)   menu_entry_go( 1);

    if(menu_key_last_event == MENU_EVENT_PGUP)   menu_entry_go(-4);
    if(menu_key_last_event == MENU_EVENT_PGDOWN) menu_entry_go( 4);

    menu_redraw();

    xTimerChangePeriod( menu_key_repeat_timer, pdMS_TO_TICKS(100), 0);
    xTimerStart( menu_key_repeat_timer, 0 );
  }
}
  
void menu_stop_repeat(void) {
  xTimerStop(menu_key_repeat_timer, 0);
  menu_key_last_event = -1;
}

void menu_do(int event) {
  //
  if(menu_dialog_is_open()) {
    // if the dialog is open, then any key event will close it
    if((event >= MENU_EVENT_UP) && (event <= MENU_EVENT_BACK))
      menu_dialog_close();

    return;
  }

  // -1 is a timer event used to scroll the current file name if it's to long
  // for the OSD
  if(event < 0) {
    if(cfg) {
      if(menu_state->type == MENU_TYPE_FILESELECTOR)
	menu_fs_scroll_entry();
    }
      
    return;
  }
  
  menu_debugf("do %d", event);

  // a core error message takes the keys while it's open: OK closes it
  if(event && core_err.open) {
    if(event == MENU_EVENT_SELECT || event == MENU_EVENT_BACK || event == MENU_EVENT_TOGGLE ||
       event == MENU_EVENT_LEFT || event == MENU_EVENT_RIGHT) {
      menu_core_error_close();
      if(osd_is_visible()) menu_redraw();
    }
    return;
  }

  // the "Apply and restart?" question takes the keys while it's open
  if(event && confirm.open) {
    if(event == MENU_EVENT_LEFT || event == MENU_EVENT_RIGHT ||
       event == MENU_EVENT_UP   || event == MENU_EVENT_DOWN)
      confirm.sel ^= 1;
    else if(event == MENU_EVENT_SELECT)
      menu_confirm_choose(confirm.sel == 0);
    else if(event == MENU_EVENT_BACK)
      confirm.open = false;   // stay on the page, changes keep waiting
    else
      return;                 // e.g. key release

    if(osd_is_visible()) menu_redraw();
    return;
  }

  if(event)  {
    if(event == MENU_EVENT_TOGGLE) {
      if(!osd_is_visible())
	osd_enable(OSD_VISIBLE);
      else {
	menu_leave_all();   // saves, or asks about waiting changes first
	if(osd_is_visible()) menu_redraw();
	return;  // return now to prevent OSD from being drawn, again
      }
    }

    // a key release event just stops any repeat timer
    if(event == MENU_EVENT_KEY_RELEASE) {
      menu_stop_repeat();
      return;
    }

    // UP/DOWN PGUP and PGDOWN have a repeat
    if(event == MENU_EVENT_UP || event == MENU_EVENT_DOWN ||
       event == MENU_EVENT_PGUP || event == MENU_EVENT_PGDOWN) {
      
      if(menu_key_repeat_timer) {
	menu_key_last_event = event;
	xTimerChangePeriod( menu_key_repeat_timer, pdMS_TO_TICKS(500), 0);
	xTimerStart( menu_key_repeat_timer, 0 );
      }
    }
    
    if(event == MENU_EVENT_UP)     menu_entry_go(-1);
    if(event == MENU_EVENT_DOWN)   menu_entry_go( 1);

    if(event == MENU_EVENT_PGUP)   menu_entry_go(-4);
    if(event == MENU_EVENT_PGDOWN) menu_entry_go( 4);

    if(event == MENU_EVENT_LEFT)   menu_left_right(-1);
    if(event == MENU_EVENT_RIGHT)  menu_left_right( 1);

    if(event == MENU_EVENT_SELECT) menu_select();
    if(event == MENU_EVENT_BACK)   menu_back();
  }

  // nothing to draw once the menu has been closed
  if(osd_is_visible()) menu_redraw();
}

TimerHandle_t menu_timer_handle;
// queue to forward key press events from USB to MENU
QueueHandle_t menu_queue = NULL;

void menu_timer_enable(bool on) {
  if(on) xTimerStart(menu_timer_handle, 0);
  else   xTimerStop(menu_timer_handle, 0);
}

// a 25Hz timer that can be activated by the menu whenever animations
// are displayed and which should be updated constantly
static void menu_timer(__attribute__((unused)) TimerHandle_t pxTimer) {
  static long msg = -1;
  xQueueSendToBack(menu_queue, &msg,  ( TickType_t ) 0);
}

static const config_menu_t system_menu_main;

// check if menu is the system menu or a submenu of it
static bool menu_is_systemmenu(void) {
  menu_state_t *ms = menu_state;
  
  while(ms) {
    if((ms->type == CONFIG_MENU_ENTRY_MENU) &&
       (ms->menu == &system_menu_main))
      return true;

    ms = ms->prev;
  }
  return false;
}  

/* ================= volume hotkeys ================= */

static TimerHandle_t menu_strip_timer = NULL;      // hides the volume bar
static TimerHandle_t menu_autosave_timer = NULL;   // saves after the last volume change

static void menu_strip_timeout(__attribute__((unused)) TimerHandle_t arg) {
  menu_notify(MENU_EVENT_STRIP_HIDE);
}

static void menu_autosave_timeout(__attribute__((unused)) TimerHandle_t arg) {
  menu_notify(MENU_EVENT_AUTOSAVE);
}

// the list marked role="volume", hidden or not
static config_menu_entry_t *menu_find_volume(const config_menu_t *menu) {
  for(config_menu_entry_t *e = menu->entries; e; e = e->next) {
    if(e->type == CONFIG_MENU_ENTRY_LIST && (e->list->flags & CONFIG_LIST_VOLUME)) return e;
    if(e->type == CONFIG_MENU_ENTRY_MENU) {
      config_menu_entry_t *v = menu_find_volume(e->menu);
      if(v) return v;
    }
  }
  return NULL;
}

// TV style volume bar in the bottom 16 lines, shown in the OSD's strip mode
static void menu_volume_bar(const config_list_t *list) {
  // the number sits in a slot as wide as the widest label, ending 4 pixels
  // from the right; with the speaker 6 pixels from the left both sides get
  // about the same padding. The bar fills the space between.
  u8g2_SetFont(&u8g2, u8g2_font_helvB08_tr);
  int steps = -1, pos = 0, nw = 0, value = menu_variable_get(list->id);
  const char *label = "";
  for(config_listentry_t *le = list->listentries; le; le = le->next) {
    steps++;
    if(le->value == value) { pos = steps; label = le->label; }
    int w = u8g2_GetStrWidth(&u8g2, le->label);
    if(w > nw) nw = w;
  }
  if(steps < 1) steps = 1;

  const int y = 48, ix = 6, slot_r = 124, slot_l = slot_r - nw;
  const int bx = ix + 13, bw = slot_l - 4 - bx, by = y + 5, bh = 7;
  u8g2_ClearBuffer(&u8g2);
  u8g2_DrawXBM(&u8g2, ix, y+4, 4, 8, icn_speaker_bits);
  if(pos) u8g2_DrawXBM(&u8g2, ix+5, y+4, 4, 8, icn_sound_bits);
  else    u8g2_DrawXBM(&u8g2, ix+6, y+4, 4, 8, icn_mute_bits);
  u8g2_DrawFrame(&u8g2, bx, by, bw, bh);
  if(pos) u8g2_DrawBox(&u8g2, bx+2, by+2, (bw-4)*pos/steps, bh-4);
  for(int t=1;t<steps;t++) u8g2_DrawPixel(&u8g2, bx+2+(bw-4)*t/steps, by-2);   // a tick per step
  u8g2_DrawStr(&u8g2, slot_l + (nw - u8g2_GetStrWidth(&u8g2, label) + 1) / 2, y+12, label);
  u8g2_SetFont(&u8g2, font_helvR08_te);
  u8g2_SendBuffer(&u8g2);
}

static void menu_volume_step(int dir) {
  if(!cfg) return;
  config_menu_entry_t *e = menu_find_volume(cfg->menu);
  if(!e) return;

  menu_list_step(e, dir, false);

  if(osd_is_visible())
    menu_redraw();   // the menu itself is open, no bar
  else {
    menu_volume_bar(e->list);
    osd_strip(1);
    xTimerReset(menu_strip_timer, 0);
  }

  // save a moment after the last change, not on every key press
  xTimerReset(menu_autosave_timer, 0);
}

static void menu_handle_latin1(uint8_t code) {
  // this function receives lagtin1 encoded keyboard events and
  // is meant to be used for complex text input in custom dialogs  
  if(code > 31) menu_debugf("menu_handle_latin1(%d/%c)", code, code);
  else          menu_debugf("menu_handle_latin1(%d)", code);
}

static void menu_task(__attribute__((unused)) void *parms) {
  menu_debugf("task running");

  // wait for user events
  while(1) {
    // receive events from usb    
    long cmd;
    xQueueReceive(menu_queue, &cmd, 0xffffffffUL);
    menu_debugf("command %04lx", cmd);

    if(cmd == MENU_EVENT_SYSTEM) {
      menu_debugf("system menu requested");
      menu_dialog_close();
      
      // open osd if it's not open, yet
      if(!osd_is_visible()) osd_enable(OSD_VISIBLE);

      // check if we are already in system menu
      if(!menu_is_systemmenu()) {
	menu_goto(&system_menu_main);
	menu_do(0);   // (re)draw menu
      } else
	menu_debugf("Already in system menu");
    } else

    // catch some non-user controlled events here

#ifdef ENABLE_BLUETOOTH
    if(cmd == MENU_EVENT_BLUETOOTH_CONNECTED) {      
      menu_draw_dialog("Bluetooth", "A device has been connected!");
    } else if(cmd == MENU_EVENT_BLUETOOTH_DISCONNECTED) {
      menu_draw_dialog("Bluetooth", "A device has been disconnected!");
    } else if(cmd == MENU_EVENT_BLUETOOTH_SCAN) {
      menu_draw_dialog("Bluetooth", "Searching for 10 seconds for devices ...");
    } else if(cmd == MENU_EVENT_BLUETOOTH_PIN_CODE_REQUEST) {
      menu_draw_dialog("Bluetooth", "Please enter pin '123456'");
    } else
#endif

    if(cmd == MENU_EVENT_NETWORK_GOT_IP) {
      char message[32];
      snprintf(message, sizeof(message), "IP: %s", network_ipaddr);
      menu_draw_dialog_for("Network", message, pdMS_TO_TICKS(5000));
    } else if(cmd == MENU_EVENT_NETWORK_DISCONNECTED) {
      menu_draw_dialog_for("Network", "disconnected", pdMS_TO_TICKS(3000));
    } else
  if(cmd == MENU_EVENT_USB_MOUNTED) {
      menu_debugf("USB mount event");

      vTaskDelay(100);

      static FATFS usb_fs;
      FRESULT fres;
      if ( (fres = f_mount(&usb_fs, "/usb", 1)) != FR_OK ) menu_debugf("/usb mount failed: %d", fres);
      else {
	menu_debugf("/usb mounted");
	menu_draw_dialog("USB", "A USB mass storage device has been detected!");
      }
    } else if(cmd == MENU_EVENT_USB_UMOUNTED) {
      if (f_unmount("/usb") != FR_OK )	menu_debugf("/usb unmount failed");
      else	                        menu_debugf("/usb unmounted");
      menu_draw_dialog("USB", "A USB mass storage device has been removed!");
    }

    else if(cmd == MENU_EVENT_VOLUME_DOWN || cmd == MENU_EVENT_VOLUME_UP)
      menu_volume_step(cmd == MENU_EVENT_VOLUME_UP ? 1 : -1);

    else if(cmd == MENU_EVENT_MEDIA_VOLUME_DOWN || cmd == MENU_EVENT_MEDIA_VOLUME_UP) {
      if(inifile_option_get(INIFILE_OPTION_MEDIAKEYS))
        menu_volume_step(cmd == MENU_EVENT_MEDIA_VOLUME_UP ? 1 : -1);
    }

    else if(cmd == MENU_EVENT_STRIP_HIDE) {
      if(!osd_is_visible()) osd_strip(0);   // unless the menu was opened meanwhile
    }

    else if(cmd == MENU_EVENT_AUTOSAVE) {
      if(!osd_is_visible()) menu_autosave(); // an open menu saves when it closes
    }

    else if((cmd & 0xffffff00) == MENU_EVENT_CORE_ERROR)
      menu_core_error(cmd & 0xff);

    else if((cmd & 0xffffff00) == MENU_EVENT_KEY_LATIN1)
      menu_handle_latin1(cmd & 0xff);

    else
      menu_do(cmd);
  }
}

TaskHandle_t menu_handle = NULL;

static void menu_add_companion_page(void);

void menu_init(void) {
  menu_debugf("Initializing");

  // check if a config was loaded. If no, use the legacy menu
  if(!cfg) {  
    menu_debugf("Warning: No core config found. Is this an old legacy core?");
    return;
  }

  // a config was loaded, use that
  menu_debugf("Using configured menu");

  menu_debugf("Setting up variables");
  menu_setup_variables();
    
  menu_debugf("Processing init action");
  sys_run_action_by_name("init");

  // every core's main menu ends with the companion's own settings
  menu_add_companion_page();

  menu_goto(cfg->menu);

  // create a one shot timer for key repeat
  menu_key_repeat_timer = xTimerCreate( "Key repeat timer", pdMS_TO_TICKS(500), pdFALSE,
					NULL, menu_key_repeat);

  // volume bar display time and the save after volume changes
  menu_strip_timer = xTimerCreate("Volume bar timer", pdMS_TO_TICKS(1500), pdFALSE,
				  NULL, menu_strip_timeout);
  menu_autosave_timer = xTimerCreate("Autosave timer", pdMS_TO_TICKS(2000), pdFALSE,
				     NULL, menu_autosave_timeout);
    
  // switch MCU controlled leds off
  sys_set_leds(0x00);
  
  // create a 25 Hz timer that frequently wakes the OSD thread
  // allowing for animations
  menu_timer_handle = xTimerCreate("Menu scroll timer", pdMS_TO_TICKS(40), pdTRUE,
				   NULL, menu_timer);
  
  // message queue from USB to OSD
  menu_queue = xQueueCreate(10, sizeof( long ) );
  
  // start a thread for the on screen display    
  xTaskCreate(menu_task, (char *)"menu_task", 4096, NULL, configMAX_PRIORITIES-3, &menu_handle);

  // At this point, the USB may already be ready. But since the
  // menu task wasn't ready by now, it never had a chance to be mounted properly
  if(mcu_hw_usb_msc_present()) {
    menu_debugf("triggering delayed USB init");
    menu_notify(MENU_EVENT_USB_MOUNTED);
  }
}

// queue an event for the menu task
void menu_notify(unsigned long msg) {
  if(menu_queue) 
    xQueueSendToBackFromISR(menu_queue, &msg,  ( TickType_t ) 0);
  else
    menu_debugf("menu_notify(): queue/menu task not ready!");
}

void menu_notify_ip(const char *ipaddr) {
  snprintf(network_ipaddr, sizeof(network_ipaddr), "%s", ipaddr);
  network_was_connected = true;
  menu_notify(MENU_EVENT_NETWORK_GOT_IP);
}

void menu_notify_network_disconnected(void) {
  // only report a disconnect if we had reported an ip address before
  if(!network_was_connected) return;
  network_was_connected = false;
  menu_notify(MENU_EVENT_NETWORK_DISCONNECTED);
}

void menu_joystick_state(unsigned char state, bool notify) {
  static unsigned char prev_state = 0;

  // Track the state even while the menu is closed. Otherwise the last
  // button used in the menu (e.g. left to close it) would still count as
  // pressed when the menu opens again, and its next press would be ignored.
  if(state == prev_state) return;
  prev_state = state;
  if(!notify) return;

  unsigned long msg = 0;
  menu_debugf("Joystick state change to %02x", state);

  // face buttons by position: 0x10 bottom, 0x20 right, 0x40 left, 0x80 top.
  // Left and right select, bottom goes back (SNES: Y and A select, B back;
  // PlayStation: square and circle select, cross back).
  if(state & 0x08) msg = MENU_EVENT_UP;
  if(state & 0x04) msg = MENU_EVENT_DOWN;
  if(state & 0x02) msg = MENU_EVENT_LEFT;
  if(state & 0x01) msg = MENU_EVENT_RIGHT;
  if(state & 0x60) msg = MENU_EVENT_SELECT;
  if(state & 0x10) msg = MENU_EVENT_BACK;
  if(!msg) msg = MENU_EVENT_KEY_RELEASE;
  menu_notify(msg);
}

// ===========  "single button" menu control ================

// The menu is only controlled through the single OSD hw button
// whenever no keyboard or gamepad is connected.

// three events are supported:
// <200ms: next menu entry
// >200ms and < 3s: select menu entry
// >3s: up one menu level

static TickType_t menu_button_last_event = 0;
static TimerHandle_t menu_button_timer = NULL;

static void menu_button_timer_handler(__attribute__((unused)) TimerHandle_t arg) {
  menu_debugf("button timer event");

  menu_notify(MENU_EVENT_BACK);
  menu_notify(MENU_EVENT_KEY_RELEASE);
  menu_button_last_event = 0;
}

void menu_button_state(unsigned char state) {
  menu_debugf("Button state %02x", state);

  if(mcu_hw_hid_present()) {
    if(state & 0x01) {
      // with keyboard and/or gamepad detected implement the same behaviour as
      // previous versions had
      menu_notify(MENU_EVENT_TOGGLE);
    }
  } else {
    static unsigned char prev_state = 0;

    if(state != prev_state) {
      menu_debugf("Button state change to %02x", state);

      // button has been pressed?
      if(state & 1) {
	// show the osd if it's closed
	if(!osd_is_visible())
	  menu_notify(MENU_EVENT_TOGGLE);
	else {	
	  menu_button_last_event = xTaskGetTickCount();
	  
	  if(!menu_button_timer)
	    menu_button_timer = xTimerCreate( "Button timer", pdMS_TO_TICKS(3000), pdFALSE,
					      NULL, menu_button_timer_handler);
	  
	  xTimerStart(menu_button_timer, 0);
	}
      } else if(menu_button_last_event) {
	xTimerStop(menu_button_timer, 0);

	// button action depends on press duration
	TickType_t len = xTaskGetTickCount() - menu_button_last_event;
	if(len > pdMS_TO_TICKS(200)) {
	  menu_notify(MENU_EVENT_SELECT);
	  menu_notify(MENU_EVENT_KEY_RELEASE);
	} else {
	  menu_notify(MENU_EVENT_DOWN);
	  menu_notify(MENU_EVENT_KEY_RELEASE);
	}
	menu_button_last_event = 0;
      }
	
      prev_state = state;
    }
  }
}

/* ================= system menu =================== */

/*  For simplicity and flexibility, the system menu is
    not strictly bound to use the menu structures. Instead
    it can use programmatically designed dialogs.

    Its about box is an example for this.
*/

static int about_length(void) {
  // return the number of selectable items inside the about dialog.
  return 0;
}

static void about_draw(void) {
  // three centered lines, 6 pixels below the regular rows so the block looks
  // centered in the space under the title (the descenders make it look high
  // when it's centered exactly)
  static const char *lines[] = { "FPGA Companion", "(c) 2026 the MiSTle Team", "github.com/mistle-dev" };
  int width = u8g2_GetDisplayWidth(&u8g2);
  for(int i=0;i<3;i++)
    u8g2_DrawStr(&u8g2, (width - u8g2_GetStrWidth(&u8g2, lines[i])) / 2,
		 MENU_LINE_Y + (i+1)*MENU_ENTRY_H + 6, lines[i]);
}

static const config_custom_t about = {
  .label = "About",
  .length = about_length,
  .draw = about_draw
};

static void about_func(void) {
  menu_debugf("About");

  // push a custom menu
  menu_push();
  menu_state->type = MENU_TYPE_CUSTOM;
  menu_state->custom = &about;
  menu_state->selected = 0;
  menu_state->scroll = 0;
}

static const config_action_command_t about_exec = {
  .code = CONFIG_ACTION_COMMAND_EXEC,
  .exec = about_func
};

static const config_action_t about_action = {
  .name = "about",
  .commands = (config_action_command_t*)&about_exec
};

static const config_button_t about_btn = {
  .label = "About",
  .action = (config_action_t*)&about_action
};


// the system menu is hard coded as it doesn't need to
// be modified by the running core

#ifdef ENABLE_BLUETOOTH

void btscan(void) {
  menu_notify(MENU_EVENT_BLUETOOTH_SCAN);
  bluetooth_scan();
}

static const config_action_command_t bluetooth_exec = {
  .code = CONFIG_ACTION_COMMAND_EXEC,
  .exec = btscan
};

static const config_action_t bluetooth_action = {
  .name = "btscan",
  .commands = (config_action_command_t*)&bluetooth_exec
};

static const config_button_t bluetooth_btn = {
  .label = "Scan Bluetooth",
  .action = (config_action_t*)&bluetooth_action
};

static const config_menu_entry_t system_menu_bluetooth = {
  .type = CONFIG_MENU_ENTRY_BUTTON,
  .button = (config_button_t*)&bluetooth_btn
};
#endif

// the usb core file selector
static char *core_exts[] = { "fs", "bin", NULL }; 

#if MAX_CORES < 2
#error "Setup at least 2 cores in config.h"
#endif

#ifdef DIRECT_SDC_SUPPORTED
static const config_fsel_t sdc_core_fsel = {
  .index = MAX_DRIVES+MAX_IMAGES+1,
  .label = "Load Core from SD",
  .def = "/sd/core.bin",
  .ext = (char**)core_exts
};

// second entry in main system menu
static const config_menu_entry_t system_menu_sdc_core_fsel = {
  .type = CONFIG_MENU_ENTRY_FILESELECTOR,
  .fsel = (config_fsel_t*)&sdc_core_fsel,
#ifdef ENABLE_BLUETOOTH
  .next = (config_menu_entry_t*)&system_menu_bluetooth
#endif
};
#endif

static const config_fsel_t usb_core_fsel = {
  .index = MAX_DRIVES+MAX_IMAGES+0,
  .label = "Load Core from USB",
  .def = "/usb/core.bin",
  .ext = (char**)core_exts
};

// second entry in main system menu
static const config_menu_entry_t system_menu_usb_core_fsel = {
  .type = CONFIG_MENU_ENTRY_FILESELECTOR,
  .fsel = (config_fsel_t*)&usb_core_fsel,
#ifdef DIRECT_SDC_SUPPORTED
  .next = (config_menu_entry_t*)&system_menu_sdc_core_fsel
#else
#ifdef ENABLE_BLUETOOTH
  .next = (config_menu_entry_t*)&system_menu_bluetooth
#endif
#endif
};

// first entry in main system menu
static const config_menu_entry_t system_menu_about = {
  .type = CONFIG_MENU_ENTRY_BUTTON,
  .button = (config_button_t*)&about_btn,
  .next = (config_menu_entry_t*)&system_menu_usb_core_fsel
};

// the main system menu
static const config_menu_t system_menu_main = {
  .label = "Companion",
  .entries = (config_menu_entry_t*)&system_menu_about
};

/* ================= companion settings page ================= */

/* Added to the end of every core's main menu. The settings are firmware
   options, saved with the core's settings (option lines in its ini file). */

static const char * const menu_key_labels[] = { "F12", "F11", "Scroll Lk" };
static const int          menu_key_values[] = { 0x45, 0x44, 0x47 };   // HID key codes
static const char * const padmenu_labels[]  = { "Off", "Sel+Start", "Start", "Select" };
static const char * const volkeys_labels[]  = { "Off", "F9 / F10", "Ctrl - / =" };
static const char * const onoff_labels[]    = { "Off", "On" };
static const char * const led_labels[]      = { "Blink", "On", "Off" };

static config_option_t opt_menu_key  = { "Menu key",    INIFILE_OPTION_HOTKEY,    menu_key_labels, menu_key_values, 3 };
static config_option_t opt_pad_menu  = { "Pad menu",    INIFILE_OPTION_PADMENU,   padmenu_labels,  NULL, 4 };
static config_option_t opt_vol_keys  = { "Volume keys", INIFILE_OPTION_VOLKEYS,   volkeys_labels,  NULL, 3 };
static config_option_t opt_media     = { "Media keys",  INIFILE_OPTION_MEDIAKEYS, onoff_labels,    NULL, 2 };
static config_option_t opt_pad_vol   = { "Pad volume",  INIFILE_OPTION_PADVOL,    onoff_labels,    NULL, 2 };
static config_option_t opt_led       = { "Status LED",  INIFILE_OPTION_LED,       led_labels,      NULL, 3 };

static const char *menu_network_text(void) {
  return network_was_connected ? network_ipaddr : "offline";
}
static config_info_t info_network = { "Network", menu_network_text };

static config_button_t comp_about_btn = { "About", (config_action_t*)&about_action };

// Hotkeys page
static config_menu_entry_t hk_pad_vol  = { .type = CONFIG_MENU_ENTRY_OPTION, .option = &opt_pad_vol,  .next = NULL };
static config_menu_entry_t hk_media    = { .type = CONFIG_MENU_ENTRY_OPTION, .option = &opt_media,    .next = &hk_pad_vol };
static config_menu_entry_t hk_vol_keys = { .type = CONFIG_MENU_ENTRY_OPTION, .option = &opt_vol_keys, .next = &hk_media };
static config_menu_entry_t hk_pad_menu = { .type = CONFIG_MENU_ENTRY_OPTION, .option = &opt_pad_menu, .next = &hk_vol_keys };
static config_menu_entry_t hk_menu_key = { .type = CONFIG_MENU_ENTRY_OPTION, .option = &opt_menu_key, .next = &hk_pad_menu };
static config_menu_t hotkeys_menu = { .label = "Hotkeys", .entries = &hk_menu_key };

// Companion page
static config_menu_entry_t comp_about   = { .type = CONFIG_MENU_ENTRY_BUTTON, .button = &comp_about_btn, .next = NULL };
static config_menu_entry_t comp_network = { .type = CONFIG_MENU_ENTRY_INFO,   .info = &info_network,     .next = &comp_about };
static config_menu_entry_t comp_led     = { .type = CONFIG_MENU_ENTRY_OPTION, .option = &opt_led,        .next = &comp_network };
static config_menu_entry_t comp_hotkeys = { .type = CONFIG_MENU_ENTRY_MENU,   .menu = &hotkeys_menu,     .next = &comp_led };
static config_menu_t companion_menu = { .label = "Companion", .entries = &comp_hotkeys };

static config_menu_entry_t comp_entry = { .type = CONFIG_MENU_ENTRY_MENU, .menu = &companion_menu, .next = NULL };

static void menu_add_companion_page(void) {
  config_menu_entry_t **e = &cfg->menu->entries;
  while(*e) e = &(*e)->next;
  *e = &comp_entry;
}

