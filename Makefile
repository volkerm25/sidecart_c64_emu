# Version from file
VERSION := $(shell cat version.txt)

## Tag this version
.PHONY: tag
tag:
	git tag $(VERSION) && git push origin $(VERSION) && \
	echo "Tagged: $(VERSION)"

## Build and deploy the DEV APP to a Booster over WiFi (Booster deploy API).
## The Booster must be in Development mode with the DEV APP installed.
## Override the host with: make deploy DEVICE=192.168.1.50
DEVICE ?= sidecart.local
DEV_UUID = 44444444-4444-4444-8444-444444444444
BUILD_TYPE ?= debug
PICO_TOOLCHAIN_PATH ?= $(HOME)/opt/arm-gnu-toolchain-14.2.rel1-x86_64-arm-none-eabi/bin
DEV_UF2 = dist/$(DEV_UUID)-$(VERSION).uf2
# Optional system clock / core voltage override, e.g.
#   make deploy RP_CLOCK_KHZ=250000 RP_VOLTAGE=VREG_VOLTAGE_1_15
export RP_CLOCK_KHZ RP_VOLTAGE

.PHONY: build deploy upload launch
build:
	PICO_TOOLCHAIN_PATH=$(PICO_TOOLCHAIN_PATH) ./build.sh pico_w $(BUILD_TYPE) $(DEV_UUID)

deploy: build upload launch

upload:
	@echo "Uploading $(DEV_UF2) to $(DEVICE)..."
	@curl --fail --silent --show-error --data-binary @$(DEV_UF2) \
	      -H "Content-Type: application/octet-stream" \
	      "http://$(DEVICE)/dev_upload.cgi"
	@echo

launch:
	@echo "Launching on $(DEVICE)..."
	@curl --fail --silent --show-error -o /dev/null \
	      "http://$(DEVICE)/mngr_launchapp.cgi?uuid=$(DEV_UUID)"
