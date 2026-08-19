#ifndef ROS_IMEM_H
#define ROS_IMEM_H

void imem_store(unsigned bank, unsigned addr, unsigned *cells);
void imem_add_word2(unsigned bank, unsigned addr, unsigned addend);
void dmem_store(unsigned bank, unsigned addr, unsigned value);
unsigned dmem_load(unsigned bank, unsigned addr);
void ros_enter(unsigned pcb, unsigned pc, unsigned gp, unsigned sb);
void ros_save_and_enter(unsigned pcb, unsigned pc, unsigned gp, unsigned sb);
void ros_restore(void);
void ros_set_status(unsigned v);
unsigned ros_take_status(void);

#endif
