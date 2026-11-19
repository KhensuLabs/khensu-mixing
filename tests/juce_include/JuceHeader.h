// tests/juce_include/JuceHeader.h
// Lightweight JUCE header stub for unit-test targets that cannot use
// juce_generate_juce_header (which requires a juce_add_* target).
// Includes only the JUCE modules needed by the UI design-token tests.
#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
// Camera3D.h (via StereoFieldVizComponent.h) needs juce::Vector3D / OpenGL types.
#include <juce_opengl/juce_opengl.h>
// AppController.h (via SidebarComponent.cpp) needs AudioFormatReaderSource /
// AudioTransportSource; juce_audio_utils pulls in formats + devices + processors.
#include <juce_audio_utils/juce_audio_utils.h>
