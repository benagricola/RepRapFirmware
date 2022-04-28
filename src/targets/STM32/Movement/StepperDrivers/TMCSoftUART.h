



#ifndef TMCSOFTUART_H_
#define TMCSOFTUART_H_

bool TMCSoftUARTTransfer(uint8_t driver, volatile uint8_t *WritePtr, uint32_t WriteCnt, volatile uint8_t *ReadPtr, uint32_t ReadCnt, uint32_t timeout) noexcept;
void TMCSoftUARTInit() noexcept;
void TMCSoftUARTShutdown() noexcept;
bool NeopixelDMAWrite(Pin pin, uint32_t freq, uint8_t *bits, uint32_t cnt, uint32_t zeroTime, uint32_t oneTime, uint32_t timeout) noexcept;
#endif