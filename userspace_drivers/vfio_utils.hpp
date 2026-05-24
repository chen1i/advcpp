#pragma once

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <linux/vfio.h>
#include <stdexcept>
#include <string>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

class UniqueFd {
public:
  UniqueFd() = default;
  explicit UniqueFd(int fd) : fd_(fd) {}

  UniqueFd(const UniqueFd &) = delete;
  UniqueFd &operator=(const UniqueFd &) = delete;

  UniqueFd(UniqueFd &&other) noexcept : fd_(other.release()) {}

  UniqueFd &operator=(UniqueFd &&other) noexcept {
    if (this != &other)
      reset(other.release());
    return *this;
  }

  ~UniqueFd() { reset(); }

  int get() const { return fd_; }

  int release() {
    int old = fd_;
    fd_ = -1;
    return old;
  }

  void reset(int fd = -1) {
    if (fd_ != -1)
      ::close(fd_);
    fd_ = fd;
  }

private:
  int fd_ = -1;
};

class AnonymousBuffer {
public:
  explicit AnonymousBuffer(std::size_t size) : size_(size) {
    void *ptr = ::mmap(nullptr, size_, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (ptr == MAP_FAILED) {
      int error = errno;
      throw std::runtime_error("mmap anonymous buffer: " + errno_text(error));
    }
    data_ = static_cast<std::uint8_t *>(ptr);
  }

  AnonymousBuffer(const AnonymousBuffer &) = delete;
  AnonymousBuffer &operator=(const AnonymousBuffer &) = delete;

  ~AnonymousBuffer() {
    if (data_)
      ::munmap(data_, size_);
  }

  std::uint8_t *data() const { return data_; }
  std::size_t size() const { return size_; }

  void zero() { std::memset(data_, 0, size_); }

private:
  static std::string errno_text(int error) {
    return std::strerror(error);
  }

  std::uint8_t *data_ = nullptr;
  std::size_t size_ = 0;
};

class MappedRegion {
public:
  MappedRegion(int fd, std::size_t size, off_t offset, bool writable)
      : size_(size) {
    int prot = PROT_READ | (writable ? PROT_WRITE : 0);
    void *ptr = ::mmap(nullptr, size_, prot, MAP_SHARED, fd, offset);
    if (ptr == MAP_FAILED) {
      int error = errno;
      throw std::runtime_error("mmap: " + errno_text(error));
    }
    base_ = static_cast<volatile std::uint8_t *>(ptr);
  }

  MappedRegion(const MappedRegion &) = delete;
  MappedRegion &operator=(const MappedRegion &) = delete;

  ~MappedRegion() {
    if (base_)
      ::munmap(const_cast<std::uint8_t *>(base_), size_);
  }

  std::uint8_t read8(std::size_t offset) const {
    check_range(offset, sizeof(std::uint8_t));
    return *(base_ + offset);
  }

  std::uint16_t read16(std::size_t offset) const {
    check_range(offset, sizeof(std::uint16_t));
    return *reinterpret_cast<volatile const std::uint16_t *>(base_ + offset);
  }

  std::uint32_t read32(std::size_t offset) const {
    check_range(offset, sizeof(std::uint32_t));
    return *reinterpret_cast<volatile const std::uint32_t *>(base_ + offset);
  }

  void write8(std::size_t offset, std::uint8_t value) {
    check_range(offset, sizeof(std::uint8_t));
    *(base_ + offset) = value;
  }

  void write16(std::size_t offset, std::uint16_t value) {
    check_range(offset, sizeof(std::uint16_t));
    *reinterpret_cast<volatile std::uint16_t *>(base_ + offset) = value;
  }

  void write32(std::size_t offset, std::uint32_t value) {
    check_range(offset, sizeof(std::uint32_t));
    *reinterpret_cast<volatile std::uint32_t *>(base_ + offset) = value;
  }

private:
  static std::string errno_text(int error) {
    return std::strerror(error);
  }

  void check_range(std::size_t offset, std::size_t width) const {
    if (offset > size_ || width > size_ - offset)
      throw std::out_of_range("MMIO access past mapped range");
  }

  volatile std::uint8_t *base_ = nullptr;
  std::size_t size_ = 0;
};

class DmaMapping {
public:
  DmaMapping(int container_fd, void *vaddr, std::uint64_t iova,
             std::size_t size, std::uint32_t flags)
      : container_fd_(container_fd), iova_(iova), size_(size) {
    vfio_iommu_type1_dma_map map{};
    map.argsz = sizeof(map);
    map.flags = flags;
    map.vaddr = reinterpret_cast<std::uintptr_t>(vaddr);
    map.iova = iova;
    map.size = size;

    if (::ioctl(container_fd_, VFIO_IOMMU_MAP_DMA, &map) == -1) {
      int error = errno;
      throw std::runtime_error("VFIO_IOMMU_MAP_DMA: " + errno_text(error));
    }
    active_ = true;
  }

  DmaMapping(const DmaMapping &) = delete;
  DmaMapping &operator=(const DmaMapping &) = delete;

  ~DmaMapping() { unmap_noexcept(); }

  std::uint64_t iova() const { return iova_; }
  std::size_t size() const { return size_; }

  std::uint64_t unmap() {
    if (!active_)
      return 0;

    vfio_iommu_type1_dma_unmap unmap{};
    unmap.argsz = sizeof(unmap);
    unmap.iova = iova_;
    unmap.size = size_;
    if (::ioctl(container_fd_, VFIO_IOMMU_UNMAP_DMA, &unmap) == -1) {
      int error = errno;
      throw std::runtime_error("VFIO_IOMMU_UNMAP_DMA: " + errno_text(error));
    }
    active_ = false;
    return unmap.size;
  }

private:
  static std::string errno_text(int error) {
    return std::strerror(error);
  }

  void unmap_noexcept() noexcept {
    try {
      unmap();
    } catch (...) {
    }
  }

  int container_fd_ = -1;
  std::uint64_t iova_ = 0;
  std::size_t size_ = 0;
  bool active_ = false;
};
