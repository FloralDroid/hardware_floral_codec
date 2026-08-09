# Floral hardware Codec2 store. Unsupported components are omitted at runtime,
# while the platform software store remains available as a fallback.
PRODUCT_PACKAGES += \
    android.hardware.media.c2@1.2-service-floral \
    floral_media_codecs_c2.xml

DEVICE_MANIFEST_FILE += hardware/floral/codec/service/manifest.xml
