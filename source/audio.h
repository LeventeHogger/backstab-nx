/*
 * audio.h – az android.media.AudioTrack JNI-metódusai (audio.c) a jni_impl.c táblájához
 *
 * Az audio_init()/audio_exit() a game.h-ban van; itt csak a FalsoJNI-nek átadott metódusok.
 *
 * Copyright (C) 2026 Levi (Switch port), MIT license.
 */
#pragma once

#include <stdarg.h>
#include <stdbool.h>
#include <FalsoJNI/FalsoJNI_ImplBridge.h>

// AudioTrack(int streamType, int sampleRate, int channelConfig, int audioFormat, int bufferSize, int mode)
jobject audioTrack_init(jmethodID id, va_list args);
// static int getMinBufferSize(int sampleRate, int channelConfig, int audioFormat)
jint audioTrack_getMinBufferSize(jmethodID id, va_list args);
void audioTrack_play(jmethodID id, va_list args);
void audioTrack_pause(jmethodID id, va_list args);
void audioTrack_stop(jmethodID id, va_list args);
void audioTrack_release(jmethodID id, va_list args);
// int write(byte[] data, int offset, int size) – blokkol, amíg a hangkimenet be nem fogadja az adatot
jint audioTrack_write(jmethodID id, va_list args);

// Kézi szüneteltetés (a write blokkol, amíg tart). Az audio_init() a fókuszvesztést magától kezeli
// (appletHook), ezt csak akkor kell hívni, ha máshonnan is le kell állítani a hangot.
void audio_set_suspended(bool suspended);
