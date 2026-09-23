#ifndef PCMPREROLL_H
#define PCMPREROLL_H

#include <functional>

#include "IConsumer.h"
#include "samplespack.h"

// Feed exact zero samples into the existing encoder until a newline arrives.
// Returns false on cancellation. Closed/invalid input is an error, not a start.
// The downstream display paces the loop; no display or encoder state is reset.
bool playSilenceUntilEnter(IConsumer<SamplesPack> &encoder, bool pal,
                          const std::function<bool()> &stopping,
                          int input_fd = 0);

#endif
