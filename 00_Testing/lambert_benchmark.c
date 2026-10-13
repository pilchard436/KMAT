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

static double velocity_difference(Lambert3 first, Lambert3 second) {
	Vector3 departure_difference = subtract_vec3(first.v0, second.v0);
	Vector3 arrival_difference = subtract_vec3(first.v1, second.v1);
	return fmax(mag_vec3(departure_difference), mag_vec3(arrival_difference));
}

static double endpoint_miss(Lambert3 solution, const LambertSample *sample, Body *central_body) {
	OSV departure_state = {sample->r0, solution.v0};
	OSV propagated_arrival = propagate_osv_time(departure_state, central_body, sample->time_of_flight);
	return mag_vec3(subtract_vec3(propagated_arrival.r, sample->r1)) / mag_vec3(sample->r1);
}

static double benchmark_solver(const LambertSample samples[SAMPLE_COUNT], Body *central_body, int use_izzo) {
	volatile double checksum = 0;
	clock_t start = clock();
	for(int repeat = 0; repeat < BENCHMARK_REPETITIONS; repeat++) {
		for(int i = 0; i < SAMPLE_COUNT; i++) {
			Lambert3 result = use_izzo ?
				calc_lambert3_izzo(samples[i].r0, samples[i].r1, samples[i].time_of_flight, central_body) :
				calc_lambert3(samples[i].r0, samples[i].r1, samples[i].time_of_flight, central_body);
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

	int comparable = 0;
	int status_mismatches = 0;
	double maximum_relative_velocity_error = 0;
	double maximum_existing_endpoint_miss = 0;
	double maximum_izzo_endpoint_miss = 0;
	for(int i = 0; i < SAMPLE_COUNT; i++) {
		Lambert3 existing = calc_lambert3(samples[i].r0, samples[i].r1, samples[i].time_of_flight, &sun);
		Lambert3 izzo = calc_lambert3_izzo(samples[i].r0, samples[i].r1, samples[i].time_of_flight, &sun);
		if(existing.success != LAMBERT_SUCCESS || izzo.success != LAMBERT_SUCCESS) {
			status_mismatches++;
			continue;
		}
		maximum_existing_endpoint_miss = fmax(maximum_existing_endpoint_miss,
			endpoint_miss(existing, &samples[i], &sun));
		maximum_izzo_endpoint_miss = fmax(maximum_izzo_endpoint_miss,
			endpoint_miss(izzo, &samples[i], &sun));
		comparable++;
		double scale = fmax(mag_vec3(existing.v0), mag_vec3(existing.v1));
		double relative_error = velocity_difference(existing, izzo) / fmax(scale, 1.0);
		if(relative_error > maximum_relative_velocity_error)
			maximum_relative_velocity_error = relative_error;
	}

	double existing_seconds = benchmark_solver(samples, &sun, 0);
	double izzo_seconds = benchmark_solver(samples, &sun, 1);
	printf("Samples: %d, repetitions: %d, calls per solver: %d\n",
		SAMPLE_COUNT, BENCHMARK_REPETITIONS, SAMPLE_COUNT * BENCHMARK_REPETITIONS);
	printf("Both solvers converged: %d/%d; status mismatches: %d\n",
		comparable, SAMPLE_COUNT, status_mismatches);
	printf("Maximum relative endpoint-velocity difference: %.3e\n", maximum_relative_velocity_error);
	printf("Maximum relative propagated endpoint miss: existing %.3e, Izzo %.3e\n",
		maximum_existing_endpoint_miss, maximum_izzo_endpoint_miss);
	printf("Existing solver: %.3f CPU seconds\n", existing_seconds);
	printf("Izzo solver: %.3f CPU seconds\n", izzo_seconds);
	printf("Speed ratio (existing / Izzo): %.3fx\n",
		izzo_seconds > 0 ? existing_seconds / izzo_seconds : 0);
	return 0;
}
