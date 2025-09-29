#include <stdio.h>
#include <stdlib.h>
#include <vector>
#include <algorithm>
#include <chrono>

#include "calculator-api.h"

void print_usage(const char* prog_name) {
	printf("Usage: %s <m> <k> <n>\n", prog_name);
	printf("  Performs a matrix multiplication C(m,n) = A(m,k) * B_transpose(n,k) on the NPU.\n");
	printf("  Example: %s 128 256 512\n", prog_name);
}

void fill_matrix(float* mat, int size) {
	for (int i = 0; i < size; ++i) {
		mat[i] = (float)((i % 100) / 1000.f) + 0.01f;
	}
}

void cpu_gemm_naive(
	const float* A, const float* B,
	size_t M, size_t K, size_t N,
	float* C,
	bool transA, bool transB)
{
#pragma omp parallel for collapse(2)
	for (size_t m = 0; m < M; ++m) {
		for (size_t n = 0; n < N; ++n) {
			float sum = 0.0f;
			for (size_t k = 0; k < K; ++k) {
				float a = transA ? A[k * M + m] : A[m * K + k];
				float b = transB ? B[n * K + k] : B[k * N + n];
				sum += a * b;
			}
			C[m * N + n] = sum;
		}
	}
}

int main(int argc, char* argv[]) {
	if (argc != 4 && argc != 5) {
		print_usage(argv[0]);
		printf("  [--cpu-check]  (optional) Enable CPU result verification.\n");
		return 1;
	}

	uint32_t m = atoi(argv[1]);
	uint32_t k = atoi(argv[2]);
	uint32_t n = atoi(argv[3]);

	if (m <= 0 || k <= 0 || n <= 0) {
		printf("Error: Matrix dimensions must be positive integers.\n");
		return 1;
	}

	bool cpu_check = false;
	if (argc == 5) {
		if (std::string(argv[4]) == "--cpu-check") {
			cpu_check = true;
		} else {
			printf("Unknown option: %s\n", argv[4]);
			print_usage(argv[0]);
			printf("  [--cpu-check]  (optional) Enable CPU result verification.\n");
			return 1;
		}
	}

	printf("\n=================================\n");
	printf("Starting NPU GEMM test with dimensions: C(m,n) = A(m,k) * B^T(n,k)\n");
	printf("M=%u, K=%u, N=%u\n", m, k, n);

	std::vector<float> matrix_a(m * k);
	std::vector<float> matrix_b(n * k); 
	std::vector<float> matrix_c(m * n, 0.0f);

	printf("Initializing input matrices...\n");
	fill_matrix(matrix_a.data(), m * k);
	fill_matrix(matrix_b.data(), n * k);

	printf("Calling NPU for GEMM calculation (A * B^T)...\n");

	auto start_time = std::chrono::high_resolution_clock::now();

	int result = calculator_gemm_cpp(
		matrix_a.data(),
		matrix_b.data(),
		m, k, n,
		matrix_c.data(),
		false,
		true
	);

	auto end_time = std::chrono::high_resolution_clock::now();

	auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time);
	double elapsed_ms = duration.count() / 1000.0;

	if (result == 0) {
		printf("\n[SUCCESS] NPU GEMM (A * B^T) calculation finished successfully!\n");
		printf("NPU Result matrix C (first 10 elements):\n");
		for (uint32_t i = 0; i < std::min((uint32_t)10, m * n); ++i) {
			printf("%.2f ", matrix_c[i]);
		}
		printf("\n");
	} else {
		printf("\n[FAILURE] NPU GEMM calculation failed with code %d.\n", result);
		printf("Please check logcat for more details: adb logcat -s calculator\n");
	}

	printf("\nTotal time spent on NPU call: %.3f ms\n", elapsed_ms);

	if (cpu_check) {
		printf("\nCheck if the result is correct...\n");
		std::vector<float> cpu_result(m * n, 0.0f);
		cpu_gemm_naive(matrix_a.data(), matrix_b.data(), m, k, n, cpu_result.data(), false, true);
		printf("CPU Result matrix C (first 10 elements):\n");
		for (uint32_t i = 0; i < std::min((uint32_t)10, m * n); ++i) {
			printf("%.2f ", cpu_result[i]);
		}
		printf("\n");
		int diff_count = 0;
		for (int i = 0; i < m * n; i++) {
			float denom = std::max(std::abs(cpu_result[i]), 1e-6f);
			float percent_error = std::abs(matrix_c[i] - cpu_result[i]) / denom;
			if (percent_error > 1e-4) {
				diff_count++;
			}
		}
		if (diff_count == 0) {
			printf("✓ NPU and CPU results match within tolerance (by percent error)\n");
		} else {
			printf("✗ NPU and CPU results mismatch! %d/%d, rate is %f (by percent error).\n", diff_count, m * n, diff_count / (float)(m * n));
		}
	} else {
		printf("\n[INFO] Skipping CPU result verification as per user request.\n");
	}

	return result;
}