/*
 * Copyright (c) 2019 SiFive Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <math.h>

#include <zephyr.h>
#include <misc/printk.h>
#include <device.h>
#include <sensor.h>
#include <spi.h>

#define STACK_SIZE 1024
#define ACCEL_PRIORITY CONFIG_MAIN_THREAD_PRIORITY + 1
#define RENDER_PRIORITY CONFIG_MAIN_THREAD_PRIORITY - 1

#define ACCEL_QUEUE_COUNT 1
#define RENDER_QUEUE_COUNT 1

#define DELTA_T 1 /* ms */
#define ZETA	0.1 /* Damping coefficient */

#define PI 3.1415926535
#define RADIUS	25  /* mm */
#define CIRCUMFERENCE (2 * PI * RADIUS)

#define LED_COUNT 40

#define SPI_FREQUENCY 2000000

typedef struct {
    float x; /* um/ms^2 */
    float y; /* um/ms^2 */
} vector_t;

/* Acceleration measurements are confined to the XY plane */
K_MSGQ_DEFINE(accel_queue, sizeof(vector_t), ACCEL_QUEUE_COUNT, sizeof(vector_t));

/* The position of the light is the position in millimeters about the
 * circumference of the circle. */
K_MSGQ_DEFINE(render_queue, sizeof(float), RENDER_QUEUE_COUNT, sizeof(float));

K_THREAD_STACK_DEFINE(accel_stack, STACK_SIZE);
K_THREAD_STACK_DEFINE(render_stack, STACK_SIZE);
struct k_thread vector_thread;
struct k_thread render_thread;

/* Returns the angle of position P in 32768 / 2PI rad */
inline float position_to_theta(float p) {
        return (2 * PI * p / CIRCUMFERENCE);
}

/* Returns the 0-indexed LED corresponding to position P */
int32_t position_to_led(float p) {
	int32_t led = (int32_t) (p * LED_COUNT / CIRCUMFERENCE);

	while(led < 0) {
	    led += LED_COUNT;
	}
	while(led >= LED_COUNT) {
	    led -= LED_COUNT;
	}

	return led;
}

/* Returns a unit vector in the direction of forward rotation in our
 * coordinate system */
vector_t position_to_forward(float p) {
	vector_t accel;

	/* Get the angle 90 in advance of the position. This will
	 * correspond to the vector tangent to our position pointing
	 * in the forward direction of rotation */
	float theta = position_to_theta(p + CIRCUMFERENCE/4);

	/* Convert the angle of the forward vector to cartesian
	 * coordinates */
	accel.x = cos(theta);
	accel.y = sin(theta);

	return accel;
}

inline float dot_vector(vector_t a, vector_t b) {
    return (a.x * b.x + a.y * b.y);
}

/* Compute the component of accel in the direction of the forward unit vector */
vector_t forward_component(vector_t accel, vector_t forward)
{
    /* Compute the magnitude of accel in the direction of the forward
     * unit vector */
    float dot = dot_vector(accel, forward);

    /* Scale the forward unit vector by the maginitude */
    forward.x = forward.x * dot;
    forward.y = forward.y * dot;

    return forward;
}

/* Returns the linear magnitude of an acceleration vector */
float magnitude(vector_t accel, vector_t forward)
{
	int sign = dot_vector(accel, forward) < 0 ? -1 : 1;
	return sign * sqrt(accel.x * accel.x + accel.y * accel.y);
}

/*
 * This worker reads accelerometer values and hands them to the main thread.
 */
void accel_worker(void *p1, void *p2, void *p3)
{
	struct sensor_value accel[3];
	vector_t a;
	struct device *lsm303c = device_get_binding(DT_ST_LSM303C_0_LABEL);
	int rc = 0;

	if(lsm303c == NULL) {
		printk("Failed to get LSM303C binding!\n");

		a.x = 0;
		a.y = -9.81; /* 1 gravity */
		while(1) {
			k_msgq_put(&accel_queue, &a, K_FOREVER);
		}
	}

	while(1) {
		/* Fetch the latest data */
		sensor_sample_fetch(lsm303c);

		/* Get the sample */
		sensor_channel_get(lsm303c, SENSOR_CHAN_ACCEL_XYZ, accel);

		a.x = accel[0].val1 + accel[0].val2 / 1000000;
		a.y = accel[1].val1 + accel[1].val2 / 1000000;

		a.x *= -1;

		/* Add the sample to the queue. If it's full, just drop it. */
		rc = k_msgq_put(&accel_queue, &a, K_NO_WAIT);
	}
}

/*
 * This worker renders the state of the LED ring
 */
uint32_t ctrl_buf[LED_COUNT + 2];

void render_worker(void *p1, void *p2, void *p3)
{
    float p = 0;
    int32_t led = 0;
    struct device *spi = device_get_binding(DT_SIFIVE_SPI0_1_LABEL);

    if(!spi) {
        printk("Failed to get SPI device\n");
        while(1) ;
    }

    const struct spi_config cfg = {
	.frequency = SPI_FREQUENCY,
	.operation = SPI_OP_MODE_MASTER | SPI_WORD_SET(8),
    };
    const struct spi_buf tx_buf = {
	.buf = &ctrl_buf,
	.len = (LED_COUNT + 2) * 4,
    };
    const struct spi_buf_set tx_bufs = {
	.count = 1,
	.buffers = &tx_buf,
    };

    while(1) {
	/* Get the position */
	k_msgq_get(&render_queue, &p, K_FOREVER);

	led = position_to_led(p);

	/* Zero the first word */
	ctrl_buf[0] = 0;

	/* Set all LEDs */
	for(int i = 1; i < (LED_COUNT + 2); i++) {
	    if(i == (led + 1)) {
		ctrl_buf[i] = 0xFFFFFFE1; /* Off */
	    } else {
		ctrl_buf[i] = 0x000000E0; /* On */
	    }
	}

	/* Set the last word to all ones */
	ctrl_buf[LED_COUNT + 1] = 0xFFFFFFFF;

	/* Write to LEDs */
	spi_write(spi, &cfg, &tx_bufs);
    }
}

/*
 * The main thread initializes the demo and then runs the physics.
 */
int main(void)
{
    int rc = 0;

    /* Startup workers */
    k_thread_create(&vector_thread,
		    accel_stack,
		    STACK_SIZE,
		    accel_worker,
		    NULL, NULL, NULL,
		    ACCEL_PRIORITY,
		    0,
		    K_NO_WAIT);
    k_thread_create(&render_thread,
		    render_stack,
		    STACK_SIZE,
		    render_worker,
		    NULL, NULL, NULL,
		    RENDER_PRIORITY,
		    0,
		    K_NO_WAIT);

    /* The position is encoded in linear millimeters about the
     * circumference of the circle.
     *
     * The position p = 0 is in line with the x axis of the accelerometer.
     *
     * The position p = CIRCUMFERENCE / 4 is in line with the y axis
     *  of the accelerometer.
     */
    float p = 0;

    /* The linear velocity is encoded in mm/ms */
    float v = 0;

    /* The linear acceleration is encoded in mm/ms^2 */
    float a = 0;

    vector_t accel;
    vector_t forward;

    while(1) {
	    /* Wait for one time delta */
	    k_sleep(DELTA_T);

	    /* Get the last sensor measurement */
	    k_msgq_get(&accel_queue, &accel, K_FOREVER);

	    /* Find the forward direction */
	    forward = position_to_forward(p);

	    /* Find the component of the acceleration we're
	     * experiencing in the forward direction */
	    accel = forward_component(accel, forward);

	    /* Convert to linear acceleration */
	    a = magnitude(accel, forward);

	    /* Update the velocity */
	    v = v + DELTA_T * a - (v * ZETA);

	    /* Update the position */
	    p = p + DELTA_T * v;

	    /* Render the position */
	    rc = k_msgq_put(&render_queue, &p, K_FOREVER);
    }
}
