#pragma once

#include <stdint.h>
#include "pd_types.h"

#define GPIO_PD 4     // 可选，正常5.1k下拉CC1即可。PD协议开关，经5.1k电阻连接到CC1。
// #define GPIO_CC1 3 // 可选：与GPIO_TX同时对CC线弱驱动，形成~1.7V分压器。 原理：两个GPIO都设为输出+DRIVE_CAP_0(最弱驱动)，一个输出高、一个输出低，互相较劲得到中间电压。ESP32内部上拉、下拉电阻为47K
#define GPIO_TX 3     // ^-^ 连接USB-CC1。TX发送脚，发送时切为输出，完成后切回输入。
#define GPIO_CC1_IN 0 // ^-^ 连接MUN5233集电极。RX接收：晶体管将CC的0~1.2V信号转换为3.3V逻辑电平。GPIO设为弱输出高(等效上拉)。

#define PD_BUFFER_COUNT 64

#define PD_RX_ACK_TASK_PRIO (configMAX_PRIORITIES - 1)
#define PD_PROTOCOL_TASK_PRIO (configMAX_PRIORITIES - 2)
#define PD_TX_TASK_PRIO (configMAX_PRIORITIES - 2)
#define PD_LOG_TASK_PRIO (tskIDLE_PRIORITY + 1)


//#define PD_TEST_EMARKER_CABLE
#define PD_LOG_TX_PACKETS
