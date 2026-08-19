#ifndef ROS_IMEM_H
#define ROS_IMEM_H

void imem_store(unsigned bank, unsigned addr, unsigned *cells);
void dmem_store(unsigned bank, unsigned addr, unsigned value);
void ros_enter(unsigned pcb, unsigned pc, unsigned gp, unsigned sb);

#endif
