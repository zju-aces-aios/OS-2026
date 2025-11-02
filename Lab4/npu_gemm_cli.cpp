#include <stdio.h>
#include <stdlib.h>
#include <vector>
#include <algorithm>
#include <chrono>
#include <string>

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

static int verify_naive(const std::vector<float>& A,
						const std::vector<float>& B,
						const std::vector<float>& C,
						uint32_t M, uint32_t K, uint32_t N,
						bool transY)
{
	std::vector<float> ref(M * N);
	cpu_gemm_naive(A.data(), B.data(), M, K, N, ref.data(), false, transY);

	const float tol = 1e-3f;
	int diff_count = 0;
	for (size_t i = 0; i < ref.size(); ++i) {
		float a = ref[i];
		float b = C[i];
		float diff = a - b;
		if (diff < 0) diff = -diff;
		if (diff > tol) {
			diff_count++;
			if (diff_count <= 10) {
				printf("  idx=%zu ref=%.6f npu=%.6f diff=%.6f\n", i, a, b, diff);
			}
		}
	}
	return (diff_count == 0) ? 1 : 0;
}

static int run_single_test(uint32_t m, uint32_t k, uint32_t n, bool transY, bool cpu_check,
						   const std::vector<float>& matrix_a, const std::vector<float>& matrix_b,
						   std::vector<float>& matrix_c)
{
	printf("\nCalling NPU GEMM (transY=%d)...\n", transY ? 1 : 0);
	auto start_time = std::chrono::high_resolution_clock::now();
	int result = calculator_gemm_cpp(
		matrix_a.data(),
		matrix_b.data(),
		m, k, n,
		matrix_c.data(),
		false,
		transY
	);
	auto end_time = std::chrono::high_resolution_clock::now();
	auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time);
	double elapsed_ms = duration.count() / 1000.0;

	if (result == 0) {
		printf("[SUCCESS] NPU GEMM (transY=%d) finished successfully! Time: %.3f ms\n", transY ? 1 : 0, elapsed_ms);
		printf("NPU Result matrix C (first 10 elements):\n");
		for (uint32_t i = 0; i < std::min((uint32_t)10, m * n); ++i) {
			printf("%.6f ", matrix_c[i]);
		}
		printf("\n");
	} else {
		printf("[FAILURE] NPU GEMM (transY=%d) failed with code %d.\n", transY ? 1 : 0, result);
		return result;
	}

	if (cpu_check) {
		printf("Performing CPU verification for transY=%d...\n", transY ? 1 : 0);
		int ok = verify_naive(matrix_a, matrix_b, matrix_c, m, k, n, transY);
		if (ok) {
			printf("✓ Verification PASSED (transY=%d)\n", transY ? 1 : 0);
		} else {
			printf("✗ Verification FAILED (transY=%d)\n", transY ? 1 : 0);
		}
	} else {
		printf("[INFO] Skipping CPU verification for transY=%d.\n", transY ? 1 : 0);
	}

	return result;
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

	/* Run two tests to mirror simulator behavior: transY = 0 and transY = 1 */
	std::vector<float> matrix_c0(m * n, 0.0f);
	std::vector<float> matrix_c1(m * n, 0.0f);

	int r0 = run_single_test(m, k, n, false, cpu_check, matrix_a, matrix_b, matrix_c0);
	int r1 = run_single_test(m, k, n, true, cpu_check, matrix_a, matrix_b, matrix_c1);

	printf("\n=================================\n");
	if (r0 == 0 && r1 == 0) {
		printf("All tests PASSED (transY=0 and transY=1)\n");
		return 0;
	} else {
		printf("One or more tests FAILED (r0=%d r1=%d)\n", r0, r1);
		return -1;
	}
}