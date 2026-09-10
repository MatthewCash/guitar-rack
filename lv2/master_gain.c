#include <lv2/core/lv2.h>

#include <math.h>
#include <stdbool.h>
#include <stdlib.h>

#define MASTER_GAIN_URI "urn:guitar-rack:master-gain"

typedef struct {
    const float *input_left;
    const float *input_right;
    float *output_left;
    float *output_right;
    const float *gain_db;
    float gain;
    float smoothing;
    float dc_coefficient;
    float previous_input_left;
    float previous_input_right;
    float previous_output_left;
    float previous_output_right;
    bool initialized;
} MasterGain;

static LV2_Handle instantiate(const LV2_Descriptor *descriptor, double rate,
                              const char *bundle_path, const LV2_Feature *const *features)
{
    (void)descriptor;
    (void)bundle_path;
    (void)features;
    MasterGain *self = calloc(1, sizeof(MasterGain));
    if (self != NULL) {
        self->gain = 1.0f;
        self->smoothing = 1.0f - expf(-1.0f / (0.02f * (float)rate));
        self->dc_coefficient = expf(-2.0f * (float)M_PI * 5.0f / (float)rate);
    }
    return self;
}

static void connect_port(LV2_Handle instance, uint32_t port, void *data)
{
    MasterGain *self = instance;
    switch (port) {
    case 0: self->input_left = data; break;
    case 1: self->input_right = data; break;
    case 2: self->output_left = data; break;
    case 3: self->output_right = data; break;
    case 4: self->gain_db = data; break;
    }
}

static void run(LV2_Handle instance, uint32_t count)
{
    MasterGain *self = instance;
    const float target = powf(10.0f, *self->gain_db / 20.0f);
    for (uint32_t frame = 0; frame < count; ++frame) {
        const float input_left = isfinite(self->input_left[frame]) ? self->input_left[frame] : 0.0f;
        const float input_right = isfinite(self->input_right[frame]) ? self->input_right[frame] : 0.0f;
        if (!self->initialized) {
            self->previous_input_left = input_left;
            self->previous_input_right = input_right;
            self->initialized = true;
        }
        float output_left = input_left - self->previous_input_left
            + self->dc_coefficient * self->previous_output_left;
        float output_right = input_right - self->previous_input_right
            + self->dc_coefficient * self->previous_output_right;
        self->previous_input_left = input_left;
        self->previous_input_right = input_right;
        self->previous_output_left = isfinite(output_left) ? output_left : 0.0f;
        self->previous_output_right = isfinite(output_right) ? output_right : 0.0f;
        self->gain += self->smoothing * (target - self->gain);
        output_left = self->previous_output_left * self->gain;
        output_right = self->previous_output_right * self->gain;
        self->output_left[frame] = isfinite(output_left) ? output_left : 0.0f;
        self->output_right[frame] = isfinite(output_right) ? output_right : 0.0f;
    }
}

static void cleanup(LV2_Handle instance)
{
    free(instance);
}

static const LV2_Descriptor descriptor = {
    MASTER_GAIN_URI,
    instantiate,
    connect_port,
    NULL,
    run,
    NULL,
    cleanup,
    NULL,
};

LV2_SYMBOL_EXPORT const LV2_Descriptor *lv2_descriptor(uint32_t index)
{
    return index == 0 ? &descriptor : NULL;
}
