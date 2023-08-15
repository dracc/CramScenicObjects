#include <lz4frame.h>

#include <iostream>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

const uint32_t FATX_SIZE_LIMIT = 0xFFBF6000;

const std::string CISO_MAGIC = "CISO";
const uint32_t CISO_HEADER_SIZE = 0x18;
const uint32_t CISO_BLOCK_SIZE = 0x800;
const uint32_t CISO_PLAIN_BLOCK = 0x80000000;

size_t image_offset = 0;

void detect_iso_type(std::ifstream& f) {
    try {
	f.seekg(0x18310000);
    } catch (std::exception &e) {
	std::cout << "File not big enough to be a Redump style image." << std::endl;
    }
    if (f.tellg() == 0x18310000) {
	std::string buf(20, '\0');
	f.read(&buf[0], 20);
	if (buf == "MICROSOFT*XBOX*MEDIA") {
	    image_offset = 0x18300000;
	    return;
	}
    }
    try {
	f.seekg(0x10000);
    } catch (std::exception &e) {
	std::cout << "File not big enough to be a raw XDVDFS image." << std::endl;
    }
    if (f.tellg() == 0x10000) {
	std::string buf(20, '\0');
	f.read(&buf[0], 20);
	if (buf == "MICROSOFT*XBOX*MEDIA") {
	    image_offset = 0;
	    return;
	}
    }
    std::cerr << "Could not detect ISO type." << std::endl;
    exit(1);
}

class ciso {
private:
    std::string magic;
    uint32_t ver;
    uint32_t block_size;
    uint32_t total_bytes;
    uint32_t total_blocks;
    uint32_t align;
public:
    ciso (uint32_t total_bytes) : magic(CISO_MAGIC), ver(2), block_size(CISO_BLOCK_SIZE),
				  total_bytes(total_bytes), align(2) {
	total_blocks = total_bytes/CISO_BLOCK_SIZE;
    }

    ciso (std::ifstream &f) {
	f.seekg(0, std::ios::end);
	uint32_t file_size = (int32_t)f.tellg() - image_offset;
	*this = ciso(file_size);
    }

    ciso (const char* filename) {
	std::ifstream f(filename);
	*this = ciso(f);
    }

    friend std::ostream& operator<<(std::ostream& os, ciso const& c);

    uint32_t get_block_size() { return block_size; };
    uint32_t get_total_blocks() { return total_blocks; };
    uint32_t get_align() { return align; };
    
    void write_header(std::ofstream& f) {
	f << magic;
	std::streamsize old_streamsize = f.width(2);
	f.fill(0);
	f << reinterpret_cast<const char*>(&CISO_HEADER_SIZE);
	f.width(4);
	f << reinterpret_cast<char*>(&total_bytes);
	f.width(2);
	f << reinterpret_cast<char*>(&block_size);
	f.width(1);
	f << reinterpret_cast<char*>(&ver) << reinterpret_cast<char*>(&align);
	f.width(2);
	f << '\0' << std::flush;
	f.width(old_streamsize);
    }

    void write_block_index(std::ofstream& f, std::vector<uint32_t> const& block_index) {
	std::cout << "Block_index size is: " << block_index.size()*sizeof(uint32_t) << std::endl;
	for (uint32_t i: block_index) {
	    char a = i & 0xFF;
	    char b = (i >> 4) & 0xFF;
	    char c = (i >> 8) & 0xFF;
	    char d = (i >> 12);
	    f.write(&a, sizeof(char));
	    f.write(&b, sizeof(char));
	    f.write(&c, sizeof(char));
	    f.write(&d, sizeof(char));
	}
	f << std::flush;
    }
};

std::ostream& operator<<(std::ostream& os, ciso const& c) {
    os << "Magic:        " << c.magic << '\n'
       << "Version:      " << c.ver << '\n'
       << "Block Size:   " << c.block_size << '\n'
       << "Total Bytes:  " << c.total_bytes << '\n'
       << "Total Blocks: " << c.total_blocks << '\n'
       << "Alignment:    " << c.align << std::endl;
    return os;
}

void pad_file_size(std::ofstream& f) {
    f.seekp(0, std::ios::end);
    size_t size = f.tellp();
    std::string zero(0x400 - (size & 0x3FF), '\0');
    f << zero << std::flush;
}

void compress_iso(std::string &infile) {
    LZ4F_cctx* cctxPtr;
    auto ret = LZ4F_createCompressionContext(&cctxPtr, LZ4F_VERSION);
    if (ret != 0) {
	std::cerr << "CompressionContext creation failed, exiting...";
	exit(2);
    }
    std::ofstream fout_1(infile + ".1.cso", std::ios::binary);
    std::ofstream fout_2;
    std::ifstream fin(infile, std::ios::binary);
    std::cout << "Compressing " << infile << std::endl;
    detect_iso_type(fin);
    ciso ciso(fin);
    std::cout << ciso;
    ciso.write_header(fout_1);
    std::vector<uint32_t> block_index(ciso.get_total_blocks() + 1, 0);
    ciso.write_block_index(fout_1, block_index);

    size_t write_pos = fout_1.tellp();
    uint32_t align_b = 1 << ciso.get_align();
    uint32_t align_m = align_b - 1;

    std::string raw_data(0x800, '\0');
    std::string alignment_buffer(64, '\0');
    std::ofstream *split_fout = &fout_1;

    for (uint32_t block = 0; block < ciso.get_total_blocks(); ++block) {
	// Check if we need to split the ISO (due to FATX limitations)
	if (write_pos > FATX_SIZE_LIMIT) {
	    fout_2.open(infile + ".2.cso", std::ios::binary);
	    split_fout = &fout_2;
	    write_pos = 0;
	}

	// Write alignment
	uint32_t align = write_pos & align_m;
	if (align != 0) {
	    align = align_b - align;
	    split_fout->write(alignment_buffer.c_str(), align);
	    write_pos += align;
	}

	// Mark offset index
	block_index[block] = write_pos >> ciso.get_align();

	// Read raw data
	fin.read(&raw_data[0], ciso.get_block_size());
	size_t raw_data_size = raw_data.size();
    }

    LZ4F_freeCompressionContext(cctxPtr);
}

int main(int argc, char* argv[]) {
    std::string file(argv[1]);
    compress_iso(file);
    return 0;
}
