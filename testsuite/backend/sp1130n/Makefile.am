AM_CPPFLAGS = -I$(top_srcdir)/backend -I$(top_builddir)/include \
    -I$(top_srcdir)/include $(USB_CFLAGS) -DBACKEND_NAME=sp1130n
AM_CFLAGS = @AM_CFLAGS@ $(SP1130N_CFLAGS)
LDADD = ../../../sanei/sanei_magic.lo ../../../sanei/sanei_init_debug.lo \
    ../../../sanei/sanei_config.lo ../../../sanei/sanei_constrain_value.lo \
    $(JPEG_LIBS) $(PTHREAD_LIBS) $(MATH_LIB)
CORE = ../../../backend/sp1130n-protocol.c \
    ../../../backend/sp1130n-settings.c ../../../backend/sp1130n-jpeg.c
IMAGE = ../../../backend/sp1130n-image.c
POST = ../../../backend/sp1130n-postprocess.c
NET = ../../../backend/sp1130n-network.c ../../../backend/sp1130n-transport.c

check_PROGRAMS = test_protocol test_backend test_image test_image_alloc \
    test_postprocess test_postprocess_alloc test_usb network_fixture sp1130nctl
TESTS = test_protocol test_backend test_image test_image_alloc \
    test_postprocess test_postprocess_alloc test_usb test_network.sh

test_protocol_SOURCES = test_protocol.c $(CORE) $(NET) $(IMAGE) $(POST) \
    ../../../backend/sp1130n.c
test_protocol_LDFLAGS = -Wl,--wrap=realloc -Wl,--wrap=nanosleep
test_protocol_LDADD = $(LDADD)
test_backend_SOURCES = test_backend.c ../../../backend/sp1130n.c \
    ../../../backend/sp1130n-settings.c $(NET) $(IMAGE) $(POST)
test_backend_CPPFLAGS = $(AM_CPPFLAGS) -DSP_BACKEND_TEST_SIGNAL
test_backend_LDFLAGS = -Wl,--wrap=pthread_mutex_lock
test_backend_LDADD = $(LDADD)
test_image_SOURCES = test_image.c $(IMAGE)
test_image_alloc_SOURCES = test_image.c $(IMAGE)
test_image_alloc_CPPFLAGS = $(AM_CPPFLAGS) -DSP_IMAGE_TEST_ALLOC_FAILURE
test_image_alloc_LDFLAGS = -Wl,--wrap=malloc -Wl,--wrap=calloc
test_postprocess_SOURCES = test_postprocess.c $(POST)
test_postprocess_alloc_SOURCES = test_postprocess.c $(POST)
test_postprocess_alloc_CPPFLAGS = $(AM_CPPFLAGS) -DSP_POST_TEST_ALLOC_FAILURE
test_postprocess_alloc_LDFLAGS = -Wl,--wrap=malloc -Wl,--wrap=calloc
test_usb_SOURCES = test_usb.c ../../../backend/sp1130n-usb.c
network_fixture_SOURCES = network_fixture.c $(CORE) \
    ../../../backend/sp1130n-network.c
sp1130nctl_SOURCES = sp1130nctl.c
sp1130nctl_LDADD = ../../../backend/libsp1130n.la $(USB_LIBS) \
    ../../../sanei/sanei_usb.lo \
    $(XML_LIBS) $(LDADD)

AM_TESTS_ENVIRONMENT = \
    export SP1130N_NETWORK_BUILD='$(abs_builddir)'; \
    export SP1130N_NETWORK_LIBRARY='$(abs_top_builddir)/backend/.libs/libsane-sp1130n.so'; \
    export SP1130N_NETWORK_SHARED='$(SP1130N_SHARED)'; \
    export srcdir='$(srcdir)';
EXTRA_DIST = test_network.py test_network.sh
