#include "deckhand/plugins/effect.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace deckhand::plugins {

bool PluginEffect::load(const PluginInfo& info, double sr, std::string* err) {
  inst_ = open_plugin(info, sr, kBlock, err);
  if (!inst_) return false;
  sr_ = sr;
  for (int c = 0; c < 2; ++c) {
    in_[c].assign(kBlock, 0.0f);
    out_[c].assign(kBlock, 0.0f);
  }
  m_ = 0;
  tail_left_ = 0;
  return true;
}

void PluginEffect::set_param(int index, float norm) {
  if (!inst_ || index < 0 || index >= (int)inst_->params().size()) return;
  inst_->set_norm(inst_->params()[(std::size_t)index].id, norm);
}
float PluginEffect::get_param(int index) const {
  if (!inst_ || index < 0 || index >= (int)inst_->params().size()) return 0;
  return (float)inst_->get_norm(inst_->params()[(std::size_t)index].id);
}

bool PluginEffect::busy() const { return inst_ && (on() || m_ > 1e-4f || tail_left_ > 0); }

void PluginEffect::reset() {
  if (inst_) inst_->reset();
  m_ = 0;
  tail_left_ = 0;
}

void PluginEffect::run(float* buf, int frames) {
  if (!inst_) return;
  const bool is_on = on_.load(std::memory_order_relaxed);
  if (is_on) {
    tail_left_ = send_ ? (long long)(std::min(inst_->tail_seconds(), 10.0) * sr_) + (long long)kBlock : 0;
  }
  if (!is_on && m_ <= 1e-4f && tail_left_ <= 0) return;  // idle
  const float target = is_on ? wet_.load(std::memory_order_relaxed) : 0.0f;
  const float coef = 1.0f - std::exp(-1.0f / (0.005f * (float)sr_));  // ~5 ms smoothing
  int done = 0;
  while (done < frames) {
    const int n = std::min(frames - done, kBlock);
    float* b = buf + 2 * done;
    const bool feed = is_on || (!send_ && m_ > 1e-4f);  // insert mode feeds while fading out; send mode only rings out
    for (int i = 0; i < n; ++i) {
      in_[0][(std::size_t)i] = feed ? b[2 * i] : 0.0f;
      in_[1][(std::size_t)i] = feed ? b[2 * i + 1] : 0.0f;
    }
    if (tempo_ > 0) inst_->set_transport({tempo_, beat_ + (double)done * tempo_ / (60.0 * sr_), playing_});
    const float* ip[2] = {in_[0].data(), in_[1].data()};
    float* op[2] = {out_[0].data(), out_[1].data()};
    inst_->process(ip, op, n);
    for (int i = 0; i < n; ++i) {
      // Send mode: the tail keeps full level while it rings; insert mode: smoothed wet * on.
      m_ += ((send_ && !is_on && tail_left_ > 0) ? (wet_.load(std::memory_order_relaxed) - m_) : (target - m_)) * coef;
      const float fl = op[0][i], fr = op[1][i];
      if (send_) {
        b[2 * i] += fl * m_;
        b[2 * i + 1] += fr * m_;
      } else {
        b[2 * i] = b[2 * i] * (1.0f - m_) + fl * m_;
        b[2 * i + 1] = b[2 * i + 1] * (1.0f - m_) + fr * m_;
      }
    }
    if (!is_on && tail_left_ > 0) tail_left_ -= n;
    done += n;
  }
  if (!is_on && tail_left_ <= 0 && send_) {
    m_ = 0;
    inst_->reset();  // finished ringing: leave the unit clean for next time
  }
}

bool PluginEffect::save_state(std::vector<std::uint8_t>& out) {
  if (!inst_) return false;
  std::vector<std::uint8_t> blob;
  if (!inst_->save_state(blob)) return false;
  // [on:1][wet:float][send:1][blob...]
  out.clear();
  out.push_back(on() ? 1 : 0);
  const float w = wet();
  const auto* wp = reinterpret_cast<const std::uint8_t*>(&w);
  out.insert(out.end(), wp, wp + sizeof w);
  out.push_back(send_ ? 1 : 0);
  out.insert(out.end(), blob.begin(), blob.end());
  return true;
}

bool PluginEffect::restore_state(const std::uint8_t* d, std::size_t n) {
  if (!inst_ || n < 7) return false;
  float w;
  std::memcpy(&w, d + 1, sizeof w);
  if (!inst_->restore_state(d + 6, n - 6)) return false;
  set_on(d[0] != 0);
  set_wet(w);
  send_ = d[5] != 0;
  return true;
}

}  // namespace deckhand::plugins
