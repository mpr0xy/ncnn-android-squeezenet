// Tencent is pleased to support the open source community by making ncnn available.
//
// Copyright (C) 2017 THL A29 Limited, a Tencent company. All rights reserved.
//
// Licensed under the BSD 3-Clause License (the "License"); you may not use this file except
// in compliance with the License. You may obtain a copy of the License at
//
// https://opensource.org/licenses/BSD-3-Clause
//
// Unless required by applicable law or agreed to in writing, software distributed
// under the License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
// CONDITIONS OF ANY KIND, either express or implied. See the License for the
// specific language governing permissions and limitations under the License.

#include <android/asset_manager_jni.h>
#include <android/bitmap.h>
#include <android/log.h>

#include <jni.h>

#include <string>
#include <vector>

// ncnn
#include "net.h"
#include "benchmark.h"

#include "squeezenet_v1.1.id.h"

static std::vector<std::string> squeezenet_words;
static ncnn::Net squeezenet;
static ncnn::Net squeezenet_gpu;

static std::vector<std::string> split_string(const std::string& str, const std::string& delimiter)
{
    std::vector<std::string> strings;

    std::string::size_type pos = 0;
    std::string::size_type prev = 0;
    while ((pos = str.find(delimiter, prev)) != std::string::npos)
    {
        strings.push_back(str.substr(prev, pos - prev));
        prev = pos + 1;
    }

    // To get the last substring (or only, if delimiter is not found)
    strings.push_back(str.substr(prev));

    return strings;
}

extern "C" {

JNIEXPORT jint JNI_OnLoad(JavaVM* vm, void* reserved)
{
    __android_log_print(ANDROID_LOG_DEBUG, "SqueezeNcnn", "JNI_OnLoad");

    ncnn::create_gpu_instance();

    return JNI_VERSION_1_4;
}

JNIEXPORT void JNI_OnUnload(JavaVM* vm, void* reserved)
{
    __android_log_print(ANDROID_LOG_DEBUG, "SqueezeNcnn", "JNI_OnUnload");

    ncnn::destroy_gpu_instance();
}

// public native boolean Init(AssetManager mgr);
JNIEXPORT jboolean JNICALL Java_com_tencent_squeezencnn_SqueezeNcnn_Init(JNIEnv* env, jobject thiz, jobject assetManager)
{
    AAssetManager* mgr = AAssetManager_fromJava(env, assetManager);

    // init param
    {
        int ret = squeezenet.load_param_bin(mgr, "squeezenet_v1.1.param.bin");
        if (ret != 0)
        {
            __android_log_print(ANDROID_LOG_DEBUG, "SqueezeNcnn", "load_param_bin failed");
            return JNI_FALSE;
        }
    }

    // init bin
    {
        int ret = squeezenet.load_model(mgr, "squeezenet_v1.1.bin");
        if (ret != 0)
        {
            __android_log_print(ANDROID_LOG_DEBUG, "SqueezeNcnn", "load_model failed");
            return JNI_FALSE;
        }
    }

    // use vulkan compute
    if (ncnn::get_gpu_count() != 0)
    {
        squeezenet_gpu.opt.use_vulkan_compute = true;

        {
            int ret = squeezenet_gpu.load_param_bin(mgr, "squeezenet_v1.1.param.bin");
            if (ret != 0)
            {
                __android_log_print(ANDROID_LOG_DEBUG, "SqueezeNcnn", "load_param_bin failed");
                return JNI_FALSE;
            }
        }
        {
            int ret = squeezenet_gpu.load_model(mgr, "squeezenet_v1.1.bin");
            if (ret != 0)
            {
                __android_log_print(ANDROID_LOG_DEBUG, "SqueezeNcnn", "load_model failed");
                return JNI_FALSE;
            }
        }
    }

    // init words
    {
        AAsset* asset = AAssetManager_open(mgr, "synset_words.txt", AASSET_MODE_BUFFER);
        if (!asset)
        {
            __android_log_print(ANDROID_LOG_DEBUG, "SqueezeNcnn", "open synset_words.txt failed");
            return JNI_FALSE;
        }

        int len = AAsset_getLength(asset);

        std::string words_buffer;
        words_buffer.resize(len);
        int ret = AAsset_read(asset, (void*)words_buffer.data(), len);

        AAsset_close(asset);

        if (ret != len)
        {
            __android_log_print(ANDROID_LOG_DEBUG, "SqueezeNcnn", "read synset_words.txt failed");
            return JNI_FALSE;
        }

        squeezenet_words = split_string(words_buffer, "\n");
    }

    return JNI_TRUE;
}

// public native String Detect(Bitmap bitmap, boolean use_gpu);
JNIEXPORT jstring JNICALL Java_com_tencent_squeezencnn_SqueezeNcnn_Detect(JNIEnv* env, jobject thiz, jobject bitmap, jboolean use_gpu)
{
    if (use_gpu == JNI_TRUE && ncnn::get_gpu_count() == 0)
    {
        return env->NewStringUTF("no vulkan capable gpu");
    }

    double start_time = ncnn::get_current_time();

    AndroidBitmapInfo info;
    AndroidBitmap_getInfo(env, bitmap, &info);
    int width = info.width;
    int height = info.height;
    if (width != 227 || height != 227)
        return NULL;
    if (info.format != ANDROID_BITMAP_FORMAT_RGBA_8888)
        return NULL;

    // ncnn from bitmap
    ncnn::Mat in = ncnn::Mat::from_android_bitmap(env, bitmap, ncnn::Mat::PIXEL_BGR);

    // squeezenet
    std::vector<float> cls_scores;
    {
        const float mean_vals[3] = {104.f, 117.f, 123.f};
        in.substract_mean_normalize(mean_vals, 0);

        ncnn::Extractor ex = use_gpu ? squeezenet_gpu.create_extractor() : squeezenet.create_extractor();

        ex.input(squeezenet_v1_1_param_id::BLOB_data, in);

        ncnn::Mat out;
        ex.extract(squeezenet_v1_1_param_id::BLOB_prob, out);

        cls_scores.resize(out.w);
        for (int j=0; j<out.w; j++)
        {
            cls_scores[j] = out[j];
        }
    }

    // return top class
    int top_class = 0;
    float max_score = 0.f;
    for (size_t i=0; i<cls_scores.size(); i++)
    {
        float s = cls_scores[i];
//         __android_log_print(ANDROID_LOG_DEBUG, "SqueezeNcnn", "%d %f", i, s);
        if (s > max_score)
        {
            top_class = i;
            max_score = s;
        }
    }

    const std::string& word = squeezenet_words[top_class];
    char tmp[32];
    sprintf(tmp, "%.3f", max_score);
    std::string result_str = std::string(word.c_str() + 10) + " = " + tmp;

    // +10 to skip leading n03179701
    jstring result = env->NewStringUTF(result_str.c_str());

    double elasped = ncnn::get_current_time() - start_time;
    __android_log_print(ANDROID_LOG_DEBUG, "SqueezeNcnn", "%.2fms   detect", elasped);

    return result;
}

}
