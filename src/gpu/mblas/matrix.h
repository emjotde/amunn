#pragma once

#include <memory>
#include <sstream>

#include "common/base_matrix.h"


#include <thrust/device_vector.h>

#ifdef __APPLE__
#include <boost/thread/tss.hpp>
#include <boost/pool/object_pool.hpp>
#endif

#include "gpu/types-gpu.h"

namespace GPU {
namespace mblas {

using namespace thrust::placeholders;


template <class VecType>
class TMatrix : public BaseMatrix {
  public:
    typedef typename VecType::value_type value_type;
    typedef typename VecType::iterator iterator;
    typedef typename VecType::const_iterator const_iterator;

    TMatrix()
    : rows_(0), cols_(0)
    {}

    TMatrix(size_t rows, size_t cols)
    : rows_(rows), cols_(cols), data_(rows_ * cols_)
    {}

    TMatrix(size_t rows, size_t cols, value_type val)
    : rows_(rows), cols_(cols), data_(rows_ * cols_, val)
    {}

    TMatrix(TMatrix&& m)
    : rows_(m.rows_), cols_(m.cols_), data_(std::move(m.data_)) {}

    TMatrix(const TMatrix& m) = delete;

    value_type operator()(size_t i, size_t j) const {
      return data_[i * cols_ + j];
    }

    void Set(size_t i, size_t j, float value)  {
      data_[i * cols_ + j] = value;
    }

    size_t Rows() const {
      return rows_;
    }

    size_t Cols() const {
      return cols_;
    }

    void Resize(size_t rows, size_t cols) {
      rows_ = rows;
      cols_ = cols;
      data_.resize(rows_ * cols_);
    }

    void Resize(size_t rows, size_t cols, value_type val) {
      rows_ = rows;
      cols_ = cols;
      data_.resize(rows_ * cols_, val);
    }

    void Reserve(size_t rows, size_t cols) {
      data_.reserve(rows * cols);
    }

    void Reshape(size_t rows, size_t cols) {
      rows_ = rows;
      cols_ = cols;
    }

    virtual std::string Debug() const
    {
      std::stringstream strm;
      strm << Rows() << "x" << Cols() << ":";
      for (size_t row = 0; row < Rows(); ++row) {
        float rowSum = 0;
        for (size_t col = 0; col < Cols(); ++col) {
          rowSum += (*this)(row, col);
        }
        strm << rowSum << " ";
      }
      return strm.str();
    }

    void Purge() {
      Clear();
      VecType temp;
      data_.swap(temp);
    }

    void Clear() {
      data_.clear();
      rows_ = 0;
      cols_ = 0;
    }

    VecType& GetVec() {
      return data_;
    }

    const VecType& GetVec() const {
      return data_;
    }

    value_type* data() {
      return thrust::raw_pointer_cast(data_.data());
    }

    const value_type* data() const {
      return thrust::raw_pointer_cast(data_.data());
    }

    iterator begin() {
      return data_.begin();
    }

    iterator end() {
      return data_.end();
    }

    const_iterator begin() const{
      return data_.begin();
    }

    const_iterator end() const {
      return data_.end();
    }

    size_t size() const {
      return data_.size();
    }

  static cudaStream_t& GetStream() {
    if (stream_ == nullptr) {
      assert(stream_ == nullptr);
      stream_ = new cudaStream_t;
      // cudaStreamCreateWithFlags(stream_, cudaStreamNonBlocking);
      cudaStreamCreate(stream_);
    }
    return *stream_;
  }

  private:
    size_t rows_;
    size_t cols_;
    VecType data_;
    static thread_local cudaStream_t* stream_;
};

typedef TMatrix<DeviceVector<float>> Matrix;
typedef TMatrix<DeviceVector<int>> IMatrix;


}  // namespace mblas
}  // namespace GPU
