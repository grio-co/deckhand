// Audio file decoding and tag reading through the system codecs (macOS and iOS).
// Handles MP3, AAC/M4A, FLAC, WAV, AIFF and anything else the OS can open.
#include <AudioToolbox/AudioToolbox.h>

#include <algorithm>

#include "deckhand/audio/track.hpp"

namespace deckhand {
namespace {

CFURLRef url_for(const std::string& path) {
  return CFURLCreateFromFileSystemRepresentation(nullptr, reinterpret_cast<const UInt8*>(path.data()),
                                                 static_cast<CFIndex>(path.size()), false);
}

std::string to_string(CFTypeRef ref) {
  if (!ref || CFGetTypeID(ref) != CFStringGetTypeID()) return {};
  const CFStringRef s = static_cast<CFStringRef>(ref);
  const CFIndex max = CFStringGetMaximumSizeForEncoding(CFStringGetLength(s), kCFStringEncodingUTF8) + 1;
  std::string out(static_cast<std::size_t>(max), '\0');
  if (!CFStringGetCString(s, out.data(), max, kCFStringEncodingUTF8)) return {};
  out.resize(std::char_traits<char>::length(out.c_str()));
  return out;
}

}  // namespace

std::shared_ptr<TrackAudio> decode_file(const std::string& path, std::string& error) {
  CFURLRef url = url_for(path);
  ExtAudioFileRef file = nullptr;
  const OSStatus opened = url ? ExtAudioFileOpenURL(url, &file) : -1;
  if (url) CFRelease(url);
  if (opened != noErr || !file) {
    error = "cannot open audio file (status " + std::to_string(opened) + "): " + path;
    return nullptr;
  }

  AudioStreamBasicDescription in{};
  UInt32 size = sizeof in;
  ExtAudioFileGetProperty(file, kExtAudioFileProperty_FileDataFormat, &size, &in);
  if (in.mSampleRate <= 0) {
    ExtAudioFileDispose(file);
    error = "unknown sample rate: " + path;
    return nullptr;
  }

  AudioStreamBasicDescription out{};
  out.mSampleRate = in.mSampleRate;
  out.mFormatID = kAudioFormatLinearPCM;
  out.mFormatFlags = kAudioFormatFlagsNativeFloatPacked;
  out.mChannelsPerFrame = 2;
  out.mBitsPerChannel = 32;
  out.mBytesPerFrame = 8;
  out.mFramesPerPacket = 1;
  out.mBytesPerPacket = 8;
  const OSStatus set = ExtAudioFileSetProperty(file, kExtAudioFileProperty_ClientDataFormat, sizeof out, &out);
  if (set != noErr) {
    ExtAudioFileDispose(file);
    error = "unsupported audio format (status " + std::to_string(set) + "): " + path;
    return nullptr;
  }

  auto audio = std::make_shared<TrackAudio>();
  audio->sample_rate = static_cast<std::uint32_t>(in.mSampleRate + 0.5);
  SInt64 length = 0;
  size = sizeof length;
  if (ExtAudioFileGetProperty(file, kExtAudioFileProperty_FileLengthFrames, &size, &length) == noErr && length > 0) {
    audio->pcm.reserve(static_cast<std::size_t>(length) * 2 + 8192);
  }

  const UInt32 block = 65536;
  std::size_t frames = 0;
  for (;;) {
    audio->pcm.resize((frames + block) * 2);
    AudioBufferList list;
    list.mNumberBuffers = 1;
    list.mBuffers[0].mNumberChannels = 2;
    list.mBuffers[0].mDataByteSize = block * 8;
    list.mBuffers[0].mData = audio->pcm.data() + frames * 2;
    UInt32 got = block;
    const OSStatus st = ExtAudioFileRead(file, &got, &list);
    if (st != noErr) {
      ExtAudioFileDispose(file);
      error = "decode failed (status " + std::to_string(st) + "): " + path;
      return nullptr;
    }
    if (got == 0) break;
    frames += got;
  }
  ExtAudioFileDispose(file);
  audio->pcm.resize(frames * 2);
  audio->pcm.shrink_to_fit();
  audio->frames = frames;
  if (frames == 0) {
    error = "file contains no audio: " + path;
    return nullptr;
  }
  build_waveform(*audio);
  return audio;
}

bool read_file_tags(const std::string& path, FileTags& out) {
  CFURLRef url = url_for(path);
  if (!url) return false;
  AudioFileID file = nullptr;
  const OSStatus opened = AudioFileOpenURL(url, kAudioFileReadPermission, 0, &file);
  CFRelease(url);
  if (opened != noErr || !file) return false;

  CFDictionaryRef info = nullptr;
  UInt32 size = sizeof info;
  if (AudioFileGetProperty(file, kAudioFilePropertyInfoDictionary, &size, &info) == noErr && info) {
    out.title = to_string(CFDictionaryGetValue(info, CFSTR(kAFInfoDictionary_Title)));
    out.artist = to_string(CFDictionaryGetValue(info, CFSTR(kAFInfoDictionary_Artist)));
    out.album = to_string(CFDictionaryGetValue(info, CFSTR(kAFInfoDictionary_Album)));
    CFRelease(info);
  }
  Float64 seconds = 0;
  size = sizeof seconds;
  if (AudioFileGetProperty(file, kAudioFilePropertyEstimatedDuration, &size, &seconds) == noErr) {
    out.duration_ms = static_cast<std::int64_t>(seconds * 1000.0);
  }
  AudioFileClose(file);
  return true;
}

}  // namespace deckhand
