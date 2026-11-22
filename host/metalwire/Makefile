# SPDX-License-Identifier: MIT
#
# The host half of metalwire: the Vulkan translator, its shaders and its tests.
#
#   make            metalwire_host (builds the thirteen Vulkan GLSL shaders first -- the four WP-2
#                   base functions and the nine WP-4 blur base functions -- then embeds their
#                   SPIR-V in the binary)
#   make shaders    only the SPIR-V + the generated header (for the WP-2 spike checks)
#   make test       build, then run the generator + host on lavapipe (skips without lavapipe)
#   make clean
#
# The guest half is built by the Darwin toolchain (guest/metal-plugin/build.sh), not here.
# glslangValidator is a build-time dependency of the shader step (spirv-val optional).

CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra
LDLIBS  ?= -lvulkan -lm

GLSLANG ?= glslangValidator
PYTHON  ?= python3
SPIRV_VAL ?= spirv-val

HOST_SRC = metalwire_host.c metalwire_protocol.h

SPV_DIR = build/spv
SHADERS = fixed_vert_lpf_spc.vert fixed_frag_lpf_cpf.frag std_vert1_lpf.vert luma_boost_lpf.frag \
          downsample_blur_vert_lpf.vert downsample_4_frag_lpf.frag downsample_8_frag_lpf.frag \
          narrow_blur_7_frag_lpf.frag narrow_blur_11_frag_lpf.frag narrow_blur_15_frag_lpf.frag \
          narrow_blur_19_frag_lpf.frag narrow_blur_23_frag_lpf.frag narrow_blur_27_frag_lpf.frag
SPV = $(addprefix $(SPV_DIR)/,$(addsuffix .spv,$(SHADERS)))
SHADER_HEADER = build/wp2_shaders.h

all: metalwire_host

$(SPV_DIR):
	mkdir -p $(SPV_DIR)

$(SPV_DIR)/%.spv: shaders/% | $(SPV_DIR)
	$(GLSLANG) -V --target-env vulkan1.1 -o $@ $<
	-$(SPIRV_VAL) $@

$(SHADER_HEADER): $(SPV) embed_spirv.py
	$(PYTHON) embed_spirv.py -o $@ $(SPV)

shaders: $(SHADER_HEADER)

metalwire_host: $(HOST_SRC) $(SHADER_HEADER)
	$(CC) $(CFLAGS) -o $@ metalwire_host.c $(LDLIBS)

test: metalwire_host
	PYTHONDONTWRITEBYTECODE=1 $(PYTHON) metalwire_test.py

spike: metalwire_host
	PYTHONDONTWRITEBYTECODE=1 $(PYTHON) wp2_spike.py

clean:
	rm -rf build metalwire_host

.PHONY: all shaders test spike clean
