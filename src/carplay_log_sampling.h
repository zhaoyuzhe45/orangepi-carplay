#ifndef CARPLAY_LOG_SAMPLING_H
#define CARPLAY_LOG_SAMPLING_H

/* Keep the first frames after every connection for diagnostics, then record
 * one out of every hundred events. Counters are per module or per connection. */
#define CARPLAY_LOG_INITIAL_SAMPLES 8u
#define CARPLAY_LOG_SAMPLE_INTERVAL 100u

static inline int carplay_log_should_sample(unsigned long occurrence)
{
    return occurrence != 0 &&
           (occurrence <= CARPLAY_LOG_INITIAL_SAMPLES ||
            occurrence % CARPLAY_LOG_SAMPLE_INTERVAL == 0);
}

#endif
