#include "string.h"
#include <fenv.h>
#include <libsurvive/survive.h>
#include <libsurvive/survive_reproject.h>
#include <libsurvive/survive_reproject_gen2.h>
#include <math.h>
#include <os_generic.h>

#include "sv_matrix.h"

#include "test_case.h"

#include "../generated/survive_imu.generated.h"
#include "../generated/survive_reproject.generated.h"

#ifdef HAVE_AUX_GENERATED
#include "../generated/survive_reproject.aux.generated.h"
#endif

#ifndef M_PI
#define M_PI LINMATHPI
#endif

#if !defined(__FreeBSD__) && !defined(__APPLE__)
#include "malloc.h"
#endif

#define STACK_ALLOC(nmembers) (FLT *)alloca(nmembers * sizeof(FLT))

static void rot_predict_quat(FLT t, const void *k, const SvMat *f_in, SvMat *f_out) {
	(void)k;

	const FLT *rot = SV_FLT_PTR(f_in);
	const FLT *vel = SV_FLT_PTR(f_in) + 4;
	copy3d(SV_FLT_PTR(f_out) + 4, vel);

	survive_apply_ang_velocity(SV_FLT_PTR(f_out), vel, t, rot);
}

typedef struct survive_calibration_config {
	FLT phase_scale, tilt_scale, curve_scale, gib_scale;
} survive_calibration_config;

static const survive_calibration_config default_config = {
	.phase_scale = 1., .tilt_scale = 1. / 10., .curve_scale = 1. / 10., .gib_scale = -1. / 10.};

static double next_rand(double mx) { return (float)rand() / (float)(RAND_MAX / mx) - mx / 2.; }

static size_t random_quat(LinmathQuat rtn) {
	if (rtn) {
		const LinmathEulerAngle euler = {next_rand(2 * M_PI), next_rand(2 * M_PI), next_rand(2 * M_PI)};
		quatfromeuler(rtn, euler);
		// quatcopy(rtn, LinmathQuat_Identity);
	}
	return sizeof(FLT) * 7;
}

static void random_axis_angle(LinmathAxisAngle a) {
	for (int i = 0; i < 3; i++) {
		a[i] = next_rand(2 * M_PI) - M_PI;
	}
}

typedef size_t (*generate_input)(FLT *output);
typedef void (*general_fn)(FLT *output, const FLT *input);

static void print_array(const char *label, const FLT *a, size_t t, size_t columns) {
	if (label)
		TEST_PRINTF("%32s: \t", label);
	for (int i = 0; i < t; i++) {
		if (a[i] == 0.0 || (fabs(a[i]) > .000001 && fabs(a[i]) < 1e4))
			TEST_PRINTF("%+6.6lf"
						"\t",
						(double)a[i]);
		else if (isnan(a[i])) {
			TEST_PRINTF("%6snan\t", "");
		} else {
			TEST_PRINTF("%+2.3le\t", (double)a[i]);
		}
		if (columns != 0 && i % columns == (columns - 1)) {
			TEST_PRINTF("\n%32s  \t", "");
		}
	}
	TEST_PRINTF("\n");
}
static FLT diff_array(FLT *out, const FLT *a, const FLT *b, size_t len) {
	FLT rtn = 0;
	for (int i = 0; i < len; i++) {
		rtn += (a[i] - b[i]) * (a[i] - b[i]);
		if (out)
			out[i] = fabs(a[i] - b[i]);
	}
	return sqrt(rtn) / (FLT)len;
}
static FLT print_diff_array(const char *label, const FLT *a, const FLT *b, size_t len, size_t columns) {
	FLT *array = STACK_ALLOC(len);
	FLT rtn = diff_array(array, a, b, len);
	print_array(label, array, len, columns);
	return rtn;
}

static FLT test_gen_jacobian_function(const char *name, generate_input input_fn, general_fn nongen, general_fn gen,
									  general_fn generated_jacobian, size_t outputs, size_t jac_start_idx,
									  size_t jac_length) {
	size_t inputs = input_fn(0) / sizeof(FLT);
	FLT *output_gen = STACK_ALLOC(outputs * jac_length), *output = STACK_ALLOC(outputs * jac_length);

	FLT *input = STACK_ALLOC(inputs);
	for (int n = 0; n < inputs; n++) {
		input[n] = NAN;
	}
	input_fn(input);

	for (int n = 0; n < outputs * jac_length; n++) {
		output_gen[n] = NAN;
	}
	generated_jacobian(output_gen, input);

	FLT *out = STACK_ALLOC(outputs);
	FLT *out_pt = STACK_ALLOC(outputs);
	FLT *input_copy = STACK_ALLOC(inputs);
	FLT *gen_output = STACK_ALLOC(outputs);

	const int M = 10;
	// This thing is to maintain compatibility with VS C compiler -- it chokes on FLT D[outputs][M][M];
	FLT ***D = alloca(outputs * sizeof(FLT **));
	for (int i = 0; i < outputs; i++) {
		D[i] = alloca(M * sizeof(FLT *));
		for (int j = 0; j < M; j++) {
			D[i][j] = alloca(M * sizeof(FLT));
		}
	}

	for (int i = 0; i < jac_length; i++) {
		for (int d = 0; d < M; d++) {
			for (int m = 0; m < M; m++) {
				for (int n = 0; n < outputs; n++) {
					D[n][m][d] = NAN;
				}
			}
		}
		for (int d = 0; d < M; d++) {
			FLT H = 2;
			for (int m = d; m < M; m++) {

				if (d == 0) {
					for (int n = 0; n < 2; n++) {
						memcpy(input_copy, input, sizeof(FLT) * inputs);

						int s = n == 0 ? 1 : -1;
						input_copy[jac_start_idx + i] += s * H;
						// print_array("Input", input_copy, inputs, 0);
						nongen(n == 0 ? out : out_pt, input_copy);
						gen(gen_output, input_copy);
						FLT err = diff_array(0, gen_output, n == 0 ? out : out_pt, outputs);
						if (err > 1e-5) {
							TEST_PRINTF("Gen/nongen mismatch\n");
						}

						// print_array("Output", n == 0 ? out : out_pt, outputs, 0);
					}

					for (int n = 0; n < outputs; n++) {
						D[n][m][d] = (out[n] - out_pt[n]) / (2.) / H;
					}

					H = H / 2.;
				} else {
					for (int n = 0; n < outputs; n++) {
						D[n][m][d] = (pow(4., d) * D[n][m][d - 1] - D[n][m - 1][d - 1]) / (pow(4., d) - 1);
					}
				}
			}
		}

		for (int n = 0; n < outputs; n++) {
			// TEST_PRINTF("For input %d output %d: \n", i, n);
			output[i + n * jac_length] = D[n][M - 1][M - 1];
			// print_array("D", (double*)D[n], M*M, M);
		}
	}

	TEST_PRINTF("Testing generated jacobian %s\n", name);
	print_array("inputs", input, inputs, 0);

	print_array("gen jacobian outputs", output_gen, outputs * jac_length, jac_length);
	print_array("jacobian outputs", output, outputs * jac_length, jac_length);

	FLT err = print_diff_array("Differences", output, output_gen, outputs * jac_length, jac_length);
	TEST_PRINTF("SSE: %f\n", err);
	return err;
}

typedef struct gen_function_jacobian_def {
	const char *suffix;
	general_fn jacobian;
	size_t jacobian_start_idx;
	size_t jacobian_length;
} gen_function_jacobian_def;
typedef struct gen_function_def {
	const char *name;
	general_fn generated;
	general_fn check;
	generate_input generate_inputs;
	size_t outputs;
	const struct gen_function_jacobian_def jacobians[16];
} gen_function_def;

static double run_cycles(general_fn runme, const FLT *inputs, FLT *outputs) {
	FLT runtime = 1;
	size_t cycles = 0;

	double start_gen = OGGetAbsoluteTime();
	double stop_gen = 0;
	do {
		runme(outputs, inputs);
		cycles++;
		stop_gen = OGGetAbsoluteTime();
	} while (start_gen + runtime > stop_gen);

	return cycles / (stop_gen - start_gen);
}

static FLT test_gen_function(const char *name, generate_input input_fn, general_fn nongen, general_fn generated,
							 size_t outputs) {
	FLT *output_gen = STACK_ALLOC(outputs), *output = STACK_ALLOC(outputs);

	size_t inputs = input_fn(0) / sizeof(FLT);

	FLT *input = STACK_ALLOC(inputs);
	input_fn(input);

	for (int i = 0; i < 1000; i++) {
		input_fn(input);
		generated(output_gen, input);
		nongen(output, input);

		FLT err = diff_array(0, output, output_gen, outputs);
		if (err > 1e-5) {
			TEST_PRINTF("%s eval mismatch: \n", name);
			print_array("inputs", input, inputs, 0);
			print_array("gen outputs", output, outputs, 0);
			print_array("outputs", output_gen, outputs, 0);

			FLT err = print_diff_array("Differences", output, output_gen, outputs, 0);
			TEST_PRINTF("Difference: %f\n", err);
		}
	}

	input_fn(input);
	FLT gen_hz = run_cycles(generated, input, output_gen);
	FLT hz = run_cycles(nongen, input, output);

	printf("Testing generated %-32s gen: %8.2fkz nongen: %8.2fkz\n", name, gen_hz / 1000., hz / 1000.);
	TEST_PRINTF("Testing generated %-32s gen: %8.2fkz nongen: %8.2fkz\n", name, gen_hz / 1000., hz / 1000.);
	print_array("inputs", input, inputs, 0);
	print_array("gen outputs", output, outputs, 0);
	print_array("outputs", output_gen, outputs, 0);

	FLT err = print_diff_array("Differences", output, output_gen, outputs, 0);
	TEST_PRINTF("Difference: %f\n", err);

	return err;
}

static int test_gen_function_def(const gen_function_def *def) {
	bool failed = false;
	FLT err = test_gen_function(def->name, def->generate_inputs, def->check, def->generated, def->outputs);
	failed |= err > 1e-5;
	for (int i = 0; i < 16 && def->jacobians[i].jacobian; i++) {
		const gen_function_jacobian_def *jdef = &def->jacobians[i];
		char name[64] = {0};
		strcat(name, def->name);
		strcat(name, "_");
		strcat(name, jdef->suffix);
		failed |= test_gen_jacobian_function(name, def->generate_inputs, def->check, def->generated, jdef->jacobian,
											 def->outputs, jdef->jacobian_start_idx, jdef->jacobian_length) > 1e-5;
	}
	return failed ? -1 : 0;
}

static size_t random_quat_quat(FLT *output) {
	if (output) {
		random_quat(output);
		random_quat(output + 4);
	}
	return sizeof(FLT) * 8;
}

static void general_gen_quatrotateabout(FLT *out, const FLT *input) { gen_quatrotateabout(out, input, input + 4); }
static void general_quatrotateabout(FLT *out, const FLT *input) { quatrotateabout(out, input, input + 4); }
static void general_gen_quatrotateabout_jac_q1(FLT *out, const FLT *input) {
	gen_quatrotateabout_jac_q1(out, input, input + 4);
}
static void general_gen_quatrotateabout_jac_q2(FLT *out, const FLT *input) {
	gen_quatrotateabout_jac_q2(out, input, input + 4);
}

gen_function_def quatrotateabout_def = {
	.name = "quatrotateabout",
	.generated = general_gen_quatrotateabout,
	.check = general_quatrotateabout,
	.generate_inputs = random_quat_quat,
	.outputs = 4,
	.jacobians = {{.suffix = "q1", .jacobian = general_gen_quatrotateabout_jac_q1, .jacobian_length = 4},
				  {.suffix = "q2",
				   .jacobian = general_gen_quatrotateabout_jac_q2,
				   .jacobian_length = 4,
				   .jacobian_start_idx = 4}}};

TEST(Generated, quatrotateabout) { return test_gen_function_def(&quatrotateabout_def); }

static void general_quatrotatevector(FLT *out, const FLT *input) { quatrotatevector(out, input, input + 4); }
static void general_gen_quatrotatevector(FLT *out, const FLT *input) { gen_quatrotatevector(out, input, input + 4); }
static void general_gen_quatrotatevector_jac_q(FLT *out, const FLT *input) {
	gen_quatrotatevector_jac_q(out, input, input + 4);
}
static void general_gen_quatrotatevector_jac_pt(FLT *out, const FLT *input) {
	gen_quatrotatevector_jac_pt(out, input, input + 4);
}

static size_t random_quat_vec3(FLT *output) {
	if (output) {
		random_quat(output);
		random_axis_angle(output + 4);
	}
	return sizeof(FLT) * 7;
}

gen_function_def quatrotatevector_def = {
	.name = "quatrotatevector",
	.generated = general_gen_quatrotatevector,
	.check = general_quatrotatevector,
	.generate_inputs = random_quat_vec3,
	.outputs = 3,
	.jacobians = {{.suffix = "q", .jacobian = general_gen_quatrotatevector_jac_q, .jacobian_length = 4},
				  {.suffix = "pt",
				   .jacobian = general_gen_quatrotatevector_jac_pt,
				   .jacobian_length = 3,
				   .jacobian_start_idx = 4}}};

TEST(Generated, quatrotatevector) { return test_gen_function_def(&quatrotatevector_def); }

SurvivePose random_pose() {
	const LinmathEulerAngle euler = {next_rand(2 * M_PI), next_rand(2 * M_PI), next_rand(2 * M_PI)};
	SurvivePose rtn = {.Pos = {next_rand(10), next_rand(10), next_rand(10)}};
	quatfromeuler(rtn.Rot, euler);
	return rtn;
}

void random_pose_into(FLT *out) {
	SurvivePose rtn = random_pose();
	memcpy(out, rtn.Pos, sizeof(SurvivePose));
}

LinmathAxisAnglePose random_pose_axisangle() {
	LinmathAxisAnglePose rtn = {.Pos = {next_rand(10), next_rand(10), next_rand(10)},
								.AxisAngleRot = {next_rand(2 * M_PI), next_rand(2 * M_PI), next_rand(2 * M_PI)}};
	return rtn;
}

void random_point(FLT *out) {
	out[0] = next_rand(1);
	out[1] = next_rand(1);
	out[2] = next_rand(1);
}

void random_fcal(BaseStationCal *fcal) {
	fcal->curve = next_rand(0.5);
	fcal->gibmag = next_rand(0.5);
	fcal->gibpha = next_rand(0.5);
	fcal->ogeemag = next_rand(0.5);
	fcal->ogeephase = next_rand(0.5);
	fcal->phase = next_rand(0.5);
	fcal->tilt = next_rand(0.5);
}

void print_pose(const SurvivePose *pose) {
	TEST_PRINTF("[%f %f %f] [%f %f %f %f]\n", pose->Pos[0], pose->Pos[1], pose->Pos[2], pose->Rot[0], pose->Rot[1],
				pose->Rot[2], pose->Rot[3]);
}

void print_point(const FLT *Pos) { TEST_PRINTF("[%f %f %f]\n", Pos[0], Pos[1], Pos[2]); }

#ifdef HAVE_AUX_GENERATED
void check_rotate_vector() {
	SurvivePose obj = random_pose();
	FLT pt[3];
	random_point(pt);

	int cycles = 1000;
	FLT gen_out[3], out[3];
	double start, stop;
	start = OGGetAbsoluteTime();
	for (int i = 0; i < cycles; i++) {
		gen_quatrotatevector(gen_out, obj.Rot, pt);
	}
	stop = OGGetAbsoluteTime();
	TEST_PRINTF("gen: %f %f %f (%f)\n", gen_out[0], gen_out[1], gen_out[2], stop - start);

	start = OGGetAbsoluteTime();
	for (int i = 0; i < cycles; i++) {
		quatrotatevector(out, obj.Rot, pt);
	}
	stop = OGGetAbsoluteTime();

	TEST_PRINTF("%f %f %f (%f)\n", out[0], out[1], out[2], stop - start);
}

void check_invert() {
	SurvivePose obj = random_pose();
	SurvivePose gen_inv, inv;
	gen_invert_pose(gen_inv.Pos, &obj);
	InvertPose(&inv, &obj);

	print_pose(&gen_inv);
	print_pose(&inv);
}
#endif

TEST(Generated, reproject_gen2_vals) {
	BaseStationData bsd = { 0 };
	double cal[] = {-0.047119140625, 0, 0.15478515625, 2.369140625, -0.00440216064453125, 0.4765625, -0.1766357421875};

	bsd.fcal[0].phase = 0;
	bsd.fcal[0].tilt = -0.047119140625;
	bsd.fcal[0].curve = 0.15478515625;
	bsd.fcal[0].gibpha = 2.369140625;
	bsd.fcal[0].gibmag = -0.00440216064453125;
	bsd.fcal[0].ogeephase = 0.4765625;
	bsd.fcal[0].ogeemag = -0.1766357421875;

	LinmathPoint3d xyz = {0.37831748940152643, -0.29826620924843278, -3.0530035758130878};
	double ang = survive_reproject_axis_x_gen2(&bsd.fcal[0], xyz);
	ang += M_PI * 2. * (0 + 1.) / 3.;
	TEST_PRINTF("%.16f\n", ang);
	return fabs(ang - 2.024090911337) < 1e-5 ? 0 : -1;
}


#ifdef HAVE_AUX_GENERATED
void check_apply_ang_velocity() {
	LinmathQuat qo, qi;
	random_quat(qi);
	LinmathAxisAngle v;
	random_axis_angle(v);
	FLT t = next_rand(5);
	survive_apply_ang_velocity(qo, v, t, qi);

	LinmathQuat qo2;
	gen_apply_ang_velocity(qo2, v, t, qi);

	TEST_PRINTF("Lib: " Quat_format "\n", LINMATH_QUAT_EXPAND(qo));
	TEST_PRINTF("Gen: " Quat_format "\n", LINMATH_QUAT_EXPAND(qo2));
}
#endif

extern void rot_predict_quat(FLT t, const void *k, const SvMat *f_in, SvMat *f_out);

TEST(Generated, rot_predict_quat) {
	FLT _mi[7] = { 0 };
	FLT _mo1[7] = { 0 };
	FLT _mo2[7] = { 0 };
	SvMat mi = svMat(7, 1, SV_64F, _mi);
	SvMat mo1 = svMat(7, 1, SV_64F, _mo1);
	SvMat mo2 = svMat(7, 1, SV_64F, _mo2);

	FLT t = next_rand(5);

	random_quat(_mi);
	random_axis_angle(_mi + 4);

	rot_predict_quat(t, 0, &mi, &mo1);

	gen_imu_rot_f(_mo2, t, _mi);

	TEST_PRINTF("Lib: " SurvivePose_format "\n", LINMATH_QUAT_EXPAND(_mo1), LINMATH_VEC3_EXPAND(_mo1 + 4));
	TEST_PRINTF("Gen: " SurvivePose_format "\t"
				"\n",
				LINMATH_QUAT_EXPAND(_mo2), LINMATH_VEC3_EXPAND(_mo2 + 4));

	FLT err = 0;
	for(int i = 0;i < 7;i++)
		err += fabs((_mo1[i] - _mo2[i]) * (_mo1[i] - _mo2[i]));
	return err > 1e-5 ? -1 : 0;
}

TEST(Generated, Speed) {
	SurvivePose obj2world = random_pose();

	memset(obj2world.Rot, 0, sizeof(FLT) * 4);
	obj2world.Rot[1] = 1.;

	LinmathVec3d pt;
	random_point(pt);

	SurvivePose world2lh = random_pose();
	// memset(world2lh.Rot, 0, sizeof(FLT) * 4);
	// world2lh.Rot[1] = 1.;
	// SurvivePose lh = { 0 }; lh.Rot[0] = 1.;

	survive_calibration_config config;
	BaseStationData bsd = { 0 };
	for (int i = 0; i < 10; i++)
		*((FLT *)&bsd.fcal[0].phase + i) = next_rand(0.5);

	FLT out_jac[14] = {0};

	for (int i = 0; i < 200000; i++) {
		// survive_reproject_full_jac_obj_pose
		gen_reproject_jac_obj_p(out_jac, &obj2world, pt, &world2lh, bsd.fcal);
	}

	return 0;
}

struct reproject_input {
	SurvivePose p;
	BaseStationCal fcal[2];
	SurvivePose world2lh;
	LinmathPoint3d pt;
};

size_t generate_reproject_input(FLT *out) {
	if (out != 0) {
		struct reproject_input *s = (struct reproject_input *)out;
		s->p = random_pose();
		// s->p.Rot[1] = .1;
		// quatnormalize(s->p.Rot, s->p.Rot);
		s->world2lh = random_pose();
		random_point(s->pt);
		random_fcal(s->fcal);
		random_fcal(s->fcal + 1);
	}
	return sizeof(struct reproject_input);
}

static void general_gen_reproject(FLT *out, const FLT *_input) {
	// static inline void gen_reproject(FLT* out, const SurvivePose* obj_p, const FLT* sensor_pt, const SurvivePose*
	// lh_p, const BaseStationCal* bsd) {
	struct reproject_input *input = (struct reproject_input *)_input;
	gen_reproject(out, &input->p, input->pt, &input->world2lh, input->fcal);
}
static void general_gen_reproject_gen2(FLT *out, const FLT *_input) {
	// static inline void gen_reproject(FLT* out, const SurvivePose* obj_p, const FLT* sensor_pt, const SurvivePose*
	// lh_p, const BaseStationCal* bsd) {
	struct reproject_input *input = (struct reproject_input *)_input;
	gen_reproject_gen2(out, &input->p, input->pt, &input->world2lh, input->fcal);
}

void general_gen_reproject_jac_obj(FLT *out, const FLT *_input) {
	// static inline void gen_reproject(FLT* out, const SurvivePose* obj_p, const FLT* sensor_pt, const SurvivePose*
	// lh_p, const BaseStationCal* bsd) {
	struct reproject_input *input = (struct reproject_input *)_input;
	gen_reproject_jac_obj_p(out, &input->p, input->pt, &input->world2lh, input->fcal);
}
void general_gen_reproject_gen2_jac_obj(FLT *out, const FLT *_input) {
	// static inline void gen_reproject(FLT* out, const SurvivePose* obj_p, const FLT* sensor_pt, const SurvivePose*
	// lh_p, const BaseStationCal* bsd) {
	struct reproject_input *input = (struct reproject_input *)_input;
	gen_reproject_gen2_jac_obj_p(out, &input->p, input->pt, &input->world2lh, input->fcal);
}

static void general_reproject(FLT *out, const FLT *_input) {
	struct reproject_input *input = (struct reproject_input *)_input;
	survive_reproject_full(input->fcal, &input->world2lh, &input->p, input->pt, out);
}
static void general_reproject_gen2(FLT *out, const FLT *_input) {
	struct reproject_input *input = (struct reproject_input *)_input;
	survive_reproject_full_gen2(input->fcal, &input->world2lh, &input->p, input->pt, out);
}

gen_function_def reproject_def = {
	.name = "reproject",
	.generated = general_gen_reproject,
	.check = general_reproject,
	.generate_inputs = generate_reproject_input,
	.outputs = 2,
	.jacobians = {{.suffix = "obj", .jacobian = general_gen_reproject_jac_obj, .jacobian_length = 7}}};

TEST(Generated, reproject) { return test_gen_function_def(&reproject_def); }

gen_function_def reproject_gen2_def = {
	.name = "reproject_gen2",
	.generated = general_gen_reproject_gen2,
	.check = general_reproject_gen2,
	.generate_inputs = generate_reproject_input,
	.outputs = 2,
	.jacobians = {{.suffix = "obj", .jacobian = general_gen_reproject_gen2_jac_obj, .jacobian_length = 7}}};

TEST(Generated, reproject_gen2) { return test_gen_function_def(&reproject_gen2_def); }

#ifdef HAVE_AUX_GENERATED
void check_apply_pose() {
	SurvivePose obj = random_pose();
	LinmathVec3d pt, out, gen_out;
	random_point(pt);

	gen_apply_pose_to_pt(out, &obj, pt);
	ApplyPoseToPoint(gen_out, &obj, pt);

	print_point(out);
	print_point(gen_out);	
}
#endif

static void general_gen_reproject_x_gen2(FLT *out, const FLT *_input) {
	struct reproject_input *input = (struct reproject_input *)_input;
	*out = gen_reproject_axis_x_gen2(&input->p, input->pt, &input->world2lh, input->fcal);
}

static void general_gen_reproject_x_gen2_jac_obj(FLT *out, const FLT *_input) {
	struct reproject_input *input = (struct reproject_input *)_input;
	gen_reproject_axis_x_gen2_jac_obj_p(out, &input->p, input->pt, &input->world2lh, input->fcal);
}
static void general_reproject_x_gen2(FLT *out, const FLT *_input) {
	struct reproject_input *input = (struct reproject_input *)_input;

	LinmathVec3d world_pt;
	ApplyPoseToPoint(world_pt, &input->p, input->pt);

	LinmathPoint3d t_pt;
	ApplyPoseToPoint(t_pt, &input->world2lh, world_pt);

	*out = survive_reproject_axis_x_gen2(input->fcal, t_pt);
}

gen_function_def reproject_axis_x_gen2_def = {
	.name = "reproject_axis_x_gen2",
	.generated = general_gen_reproject_x_gen2,
	.check = general_reproject_x_gen2,
	.generate_inputs = generate_reproject_input,
	.outputs = 1,
	.jacobians = {
		{.suffix = "obj", .jacobian = general_gen_reproject_x_gen2_jac_obj, .jacobian_length = 7},
	}};

TEST(Generated, reproject_axis_x) { return test_gen_function_def(&reproject_axis_x_gen2_def); }