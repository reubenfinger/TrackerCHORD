#ifndef SOURCE_ENCODERIMPL_H_
#define SOURCE_ENCODERIMPL_H_
#include "Encoder.h"
#include "Platform.h"

class EncoderImpl : Encoder
{
  public:
    virtual void setAcceleration(int acceleration) override;
    virtual void setResolution(int resolution) override;
    virtual void setReadInterval(uint32_t intervalMs) override;

    void init();
    void update();

  private:
    uint32_t encTimer;
    uint32_t readIntervalMs = 100;
    cEncoder encoderDriver;
};

#endif /* SOURCE_ENCODERIMPL_H_ */
