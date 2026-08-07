# "hey billy" wake-word detector — microWakeWord streaming model on TensorFlow Lite Micro.
# C++ (TFLM is C++), exposed to the C firmware through the extern "C" interface in wakeword.h.
# The esp-tflite-micro and ESPMicroSpeechFeatures (audio frontend) dependencies come from
# idf_component.yml. EMBED_FILES links the trained model straight into the app image — see
# models/ATTRIBUTION.md for what it is and wakeword.cpp for the _binary_..._start symbol it produces.
idf_component_register(
    SRCS "wakeword.cpp"
    INCLUDE_DIRS "."
    EMBED_FILES "models/hey_billy.tflite"
    PRIV_REQUIRES esp_common log
)
