#include "memory.hpp"
#include "compression.hpp"

#include <zlib.h>

namespace utils::compression
{
	std::string zlib::compress(const std::string& data)
	{
		memory::allocator allocator;
		unsigned long length = static_cast<unsigned long>(data.size() * 2);
		if (!length) length = 2;

		if (length < 100) length *= 10;

		const auto buffer = allocator.allocate_array<char>(length);
		if (compress2(reinterpret_cast<Bytef*>(buffer), &length,
		              reinterpret_cast<Bytef*>(const_cast<char*>(data.data())), static_cast<unsigned long>(data.size()),
		              Z_BEST_COMPRESSION) != Z_OK)
		{
			return {};
		}

		return std::string(buffer, length);
	}

	std::string zlib::decompress(const std::string& data)
	{
		z_stream stream;
		memset(&stream, 0, sizeof(stream));
		std::string buffer;

		if (inflateInit(&stream) != Z_OK)
		{
			return {};
		}

		int ret;
		memory::allocator allocator;

		auto* const dest = allocator.allocate_array<uint8_t>(CHUNK);
		auto* data_ptr = data.data();

		do
		{
			stream.avail_in = std::min(CHUNK, static_cast<unsigned int>(data.size() - (data_ptr - data.data())));
			stream.next_in = reinterpret_cast<const uint8_t*>(data_ptr);
			data_ptr += stream.avail_in;

			do
			{
				stream.avail_out = CHUNK;
				stream.next_out = dest;

				ret = inflate(&stream, Z_NO_FLUSH);
				if (ret != Z_OK && ret != Z_STREAM_END)
				{
					inflateEnd(&stream);
					return {};
				}

				buffer.append(reinterpret_cast<const char*>(dest), CHUNK - stream.avail_out);
			}
			while (stream.avail_out == 0);
		}
		while (ret != Z_STREAM_END);

		inflateEnd(&stream);
		return buffer;
	}

#ifdef ENABLE_ZSTD
	std::string zstd::compress(const std::string& data)
	{
		memory::allocator allocator;
		const auto bound = ZSTD_compressBound(data.size());

		auto* buffer = allocator.allocate_array<char>(bound);
		const auto size = ZSTD_compress(buffer, bound, data.data(), data.size(), ZSTD_maxCLevel());

		return std::string(buffer, size);
	}

	std::string zstd::decompress(const std::string& data)
	{
		memory::allocator allocator;
		const auto bound = size_t(ZSTD_getFrameContentSize(data.data(), data.size()));

		auto* buffer = allocator.allocate_array<char>(bound);
		const auto size = ZSTD_decompress(buffer, bound, data.data(), data.size());

		return std::string(buffer, size);
	}
#endif
}
