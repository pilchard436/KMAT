#include "orbitlib_transfer.h"
#include "geometrylib.h"

#include <math.h>
#include <stdio.h>
#include <time.h>

#define SAMPLE_COUNT 96
#define BENCHMARK_REPETITIONS 1000

typedef struct LambertSample {
	Vector3 r0;
	Vector3 r1;
	double time_of_flight;
} LambertSample;

static void create_samples(LambertSample samples[SAMPLE_COUNT], Body *central_body) {
	for(int i = 0; i < SAMPLE_COUNT; i++) {
		double r0 = 1e10 * (1 + (i % 17) * 0.11);
		double r1 = r0 * (1.08 + (i % 11) * 0.047);
		double angle = 0.18 + (i % 120) * 0.045;
		double inclination = (i % 15 - 7) * (M_PI / 180);
		double semimajor_axis = (r0 + r1) / 2;
		double hohmann_time = M_PI * sqrt(pow(semimajor_axis, 3) / central_body->mu);
		double time_scale = 0.4 + (i % 100) * 0.012;
		samples[i] = (LambertSample) {
			.r0 = vec3(r0, 0, 0),
			.r1 = vec3(r1 * cos(angle), r1 * sin(angle) * cos(inclination),
				r1 * sin(angle) * sin(inclination)),
			.time_of_flight = hohmann_time * time_scale
		};
	}
}

static double endpoint_miss(Lambert3 solution, const LambertSample *sample, Body *central_body) {
	OSV departure_state = {sample->r0, solution.v0};
	OSV propagated_arrival = propagate_osv_time(departure_state, central_body, sample->time_of_flight);
	return mag_vec3(subtract_vec3(propagated_arrival.r, sample->r1)) / mag_vec3(sample->r1);
}

static double benchmark_solver(const LambertSample samples[SAMPLE_COUNT], Body *central_body) {
	volatile double checksum = 0;
	clock_t start = clock();
	for(int repeat = 0; repeat < BENCHMARK_REPETITIONS; repeat++) {
		for(int i = 0; i < SAMPLE_COUNT; i++) {
			Lambert3 result = calc_lambert3(samples[i].r0, samples[i].r1, samples[i].time_of_flight, central_body);
			checksum += result.v0.x + result.v1.y;
		}
	}
	clock_t end = clock();
	if(checksum == 0) printf("Unexpected zero benchmark checksum\n");
	return (double)(end - start) / CLOCKS_PER_SEC;
}

int main(void) {
	Body sun = {.mu = 1.3271244004193938e20};
	LambertSample samples[SAMPLE_COUNT];
	create_samples(samples, &sun);

	int converged = 0;
	double maximum_endpoint_miss = 0;
	for(int i = 0; i < SAMPLE_COUNT; i++) {
		Lambert3 solution = calc_lambert3(samples[i].r0, samples[i].r1, samples[i].time_of_flight, &sun);
		if(solution.success != LAMBERT_SUCCESS) continue;
		converged++;
		maximum_endpoint_miss = fmax(maximum_endpoint_miss, endpoint_miss(solution, &samples[i], &sun));
	}

	double seconds = benchmark_solver(samples, &sun);
	printf("Samples: %d, repetitions: %d, calls: %d\n",
		SAMPLE_COUNT, BENCHMARK_REPETITIONS, SAMPLE_COUNT * BENCHMARK_REPETITIONS);
	printf("Converged: %d/%d\n", converged, SAMPLE_COUNT);
	printf("Maximum relative propagated endpoint miss: %.3e\n", maximum_endpoint_miss);
	printf("Lambert solver: %.3f CPU seconds\n", seconds);
	return 0;
}
