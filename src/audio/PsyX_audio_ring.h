#ifndef PSYX_AUDIO_RING_H
#define PSYX_AUDIO_RING_H

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace psyx::audio
{

struct StereoFrame
{
	double left;
	double right;
};

class CanonicalRing
{
public:
	explicit CanonicalRing(size_t capacity)
		: frames_(std::max<size_t>(capacity, 2)),
		  read_(0),
		  write_(0),
		  size_(0)
	{
	}

	size_t capacity() const { return frames_.size(); }
	size_t size() const { return size_; }
	size_t freeSpace() const { return frames_.size() - size_; }

	size_t write(const StereoFrame* source, size_t count)
	{
		const size_t accepted = std::min(count, freeSpace());
		for (size_t i = 0; i < accepted; ++i)
		{
			frames_[write_] = source[i];
			write_ = (write_ + 1) % frames_.size();
		}
		size_ += accepted;
		return accepted;
	}

	size_t writeInterleaved(const int16_t* source, size_t count)
	{
		const size_t accepted = std::min(count, freeSpace());
		for (size_t i = 0; i < accepted; ++i)
		{
			frames_[write_] = StereoFrame{
				static_cast<double>(source[i * 2]),
				static_cast<double>(source[i * 2 + 1])};
			write_ = (write_ + 1) % frames_.size();
		}
		size_ += accepted;
		return accepted;
	}

	size_t writeInterleaved(const double* source, size_t count)
	{
		const size_t accepted = std::min(count, freeSpace());
		for (size_t i = 0; i < accepted; ++i)
		{
			frames_[write_] = StereoFrame{source[i * 2], source[i * 2 + 1]};
			write_ = (write_ + 1) % frames_.size();
		}
		size_ += accepted;
		return accepted;
	}

	bool read(StereoFrame& frame)
	{
		if (size_ == 0)
			return false;
		frame = frames_[read_];
		read_ = (read_ + 1) % frames_.size();
		--size_;
		return true;
	}

	void clear()
	{
		read_ = 0;
		write_ = 0;
		size_ = 0;
	}

private:
	std::vector<StereoFrame> frames_;
	size_t read_;
	size_t write_;
	size_t size_;
};

} // namespace psyx::audio

#endif
