# These names are part of the build interface. V3D is probed at boot by
# default and falls back to CPU rendering; V3D=0 builds the CPU renderer only.
# Missing board configuration and Wi-Fi assets are reported at boot.
V3D ?= 1
WIFI ?= 1
ETHERNET ?= 1
STORAGE ?= 1
STORAGE_VALIDATION ?= 0
SD_BASE_CLOCK_HZ ?= 0
STORAGE_PARTITION ?= -1

ifneq ($(filter-out 0 1,$(V3D) $(WIFI) $(ETHERNET) $(STORAGE) $(STORAGE_VALIDATION)),)
$(error V3D, WIFI, ETHERNET, STORAGE and STORAGE_VALIDATION must be 0 or 1)
endif
ifneq ($(words $(STORAGE_PARTITION)),1)
$(error STORAGE_PARTITION must be -1 (unconfigured) or a primary index 0..3)
endif
ifneq ($(filter-out -1 0 1 2 3,$(STORAGE_PARTITION)),)
$(error STORAGE_PARTITION must be -1 (unconfigured) or a primary index 0..3)
endif
ifeq ($(STORAGE_VALIDATION),1)
ifeq ($(STORAGE),0)
$(error STORAGE_VALIDATION=1 requires STORAGE=1)
endif
ifeq ($(SD_BASE_CLOCK_HZ),0)
$(error STORAGE_VALIDATION=1 requires the confirmed SD_BASE_CLOCK_HZ)
endif
ifeq ($(STORAGE_PARTITION),-1)
$(error STORAGE_VALIDATION=1 requires STORAGE_PARTITION=0..3)
endif
endif

CPPFLAGS += -Iinclude -DVANTAGE_V3D=$(V3D) -DVANTAGE_WIFI=$(WIFI)
CPPFLAGS += -DVANTAGE_ETHERNET=$(ETHERNET)
CPPFLAGS += -DVANTAGE_STORAGE=$(STORAGE)
CPPFLAGS += -DVANTAGE_WIFI_ASSETS=$(if $(strip $(WIFI_FIRMWARE_C)),1,0)
CPPFLAGS += -DVANTAGE_STORAGE_VALIDATION=$(STORAGE_VALIDATION)
CPPFLAGS += -DSD_BASE_CLOCK_HZ=$(SD_BASE_CLOCK_HZ) -DSTORAGE_PARTITION=$(STORAGE_PARTITION)
