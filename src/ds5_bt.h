#ifndef DS5_BT_H
#define DS5_BT_H
// All calls and callbacks stay on core1 (CYW43 polling context).
void ds5_bt_init(void);
void ds5_bt_task(void);
#endif
