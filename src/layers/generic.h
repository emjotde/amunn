#pragma once

#include "marian.h"

#include "data/shortlist.h"
#include "layers/factory.h"

namespace marian {
namespace mlp {
/**
 * @brief Activation functions
 */
enum struct act : int { linear, tanh, sigmoid, ReLU, LeakyReLU, PReLU, swish };
}  // namespace mlp
}  // namespace marian

YAML_REGISTER_TYPE(marian::mlp::act, int)

namespace marian {

// Each layer consists of LayerBase and IXXXLayer which defines one or more apply()
// functions for the respective layer type (different layers may require different signatures).
// This base class contains configuration info for creating parameters and executing apply().
class LayerBase {
protected:
  Ptr<ExpressionGraph> graph_;
  Ptr<Options> options_;

public:
  LayerBase(Ptr<ExpressionGraph> graph, Ptr<Options> options)
      : graph_(graph), options_(options) {}

  template <typename T>
  T opt(const std::string key) {
    return options_->get<T>(key);
  }

  template <typename T>
  T opt(const std::string key, T defaultValue) {
    return options_->get<T>(key, defaultValue);
  }
};

// Simplest layer interface: Unary function
struct IUnaryLayer {
  virtual Expr apply(Expr) = 0;
  virtual Expr apply(const std::vector<Expr>&) = 0;
};

// Embedding from corpus sub-batch to (emb, mask)
struct IEmbeddingLayer {
  virtual std::tuple<Expr/*embeddings*/, Expr/*mask*/> apply(Ptr<data::SubBatch> subBatch) const = 0;
  // alternative version from index vector, and with batch dim
  virtual Expr apply(const std::vector<IndexType>& embIdx, int dimBatch, int dimBeam) const = 0;
};

// @HACK: Frank's quick implementation of factored outputs. To be re-thought once it works.
// Output layer returns a Logits object, which is able to compute some things on the fly
// for factored embeddings.
class Logits {
public:
    Logits(Expr logits) : logits_(logits) {
    }
    Expr getLogits() const {
      return logits_;
    }
private:
    Expr logits_;
};

// Unary function that returns a Logits object
struct IUnaryLogitLayer {
  virtual Logits apply(Expr) = 0;
  virtual Logits apply(const std::vector<Expr>&) = 0;
};

class EmbeddingFactorMapping;

namespace mlp {

class Dense : public LayerBase, public IUnaryLayer {
public:
  Dense(Ptr<ExpressionGraph> graph, Ptr<Options> options)
      : LayerBase(graph, options) {}

  Expr apply(const std::vector<Expr>& inputs) override {
    ABORT_IF(inputs.empty(), "No inputs");

    auto name = opt<std::string>("prefix");
    auto dim = opt<int>("dim");

    auto useLayerNorm = opt<bool>("layer-normalization", false);
    auto useNematusNorm = opt<bool>("nematus-normalization", false);
    auto activation = opt<act>("activation", act::linear);

    auto g = graph_;

    std::vector<Expr> outputs;
    size_t i = 0;

    std::string num;
    for(auto&& in : inputs) {
      if(inputs.size() > 1)
        num = std::to_string(i);

      Expr W = g->param(
          name + "_W" + num, {in->shape()[-1], dim}, inits::glorot_uniform);
      Expr b = g->param(name + "_b" + num, {1, dim}, inits::zeros);

      if(useLayerNorm) {
        if(useNematusNorm) {
          auto ln_s = g->param(
              name + "_ln_s" + num, {1, dim}, inits::from_value(1.f));
          auto ln_b = g->param(name + "_ln_b" + num, {1, dim}, inits::zeros);

          outputs.push_back(
              layerNorm(affine(in, W, b), ln_s, ln_b, NEMATUS_LN_EPS));
        } else {
          auto gamma = g->param(
              name + "_gamma" + num, {1, dim}, inits::from_value(1.0));

          outputs.push_back(layerNorm(dot(in, W), gamma, b));
        }
      } else {
        outputs.push_back(affine(in, W, b));
      }
      i++;
    }

    // clang-format off
    switch(activation) {
      case act::linear:    return plus(outputs);
      case act::tanh:      return tanh(outputs);
      case act::sigmoid:   return sigmoid(outputs);
      case act::ReLU:      return relu(outputs);
      case act::LeakyReLU: return leakyrelu(outputs);
      case act::PReLU:     return prelu(outputs);
      case act::swish:     return swish(outputs);
      default:             return plus(outputs);
    }
    // clang-format on
  };

  Expr apply(Expr input) override { return apply(std::vector<Expr>({input})); }
};

class Output : public LayerBase, public IUnaryLogitLayer {
private:
  Expr W_;  // parameters held by this layer
  Expr b_;
  Expr cachedShortW_;   // short-listed version, cached (cleared by clear())
  Expr cachedShortb_;   // these match the current value of shortlist_
  Ptr<EmbeddingFactorMapping > embeddingFactorMapping_;

  // optional parameters set/updated after construction
  Expr tiedParam_;
  bool transposeW_{false};
  Ptr<data::Shortlist> shortlist_;

  void lazyConstruct(int inputDim);
public:
  Output(Ptr<ExpressionGraph> graph, Ptr<Options> options)
      : LayerBase(graph, options) {
    clear();
  }

  void tieTransposed(Expr tied) {
    if (W_)
      ABORT_IF(tiedParam_.get() != tied.get(), "Tied output projection cannot be changed once weights have been created");
    else
      tiedParam_ = tied;
  }

  void setShortlist(Ptr<data::Shortlist> shortlist) {
    if (shortlist_)
      ABORT_IF(shortlist.get() != shortlist_.get(), "Output shortlist cannot be changed except after clear()");
    else {
      ABORT_IF(cachedShortW_ || cachedShortb_, "No shortlist but cached parameters??");
      shortlist_ = shortlist;
    }
    // cachedShortW_ and cachedShortb_ will be created lazily inside apply()
  }

  // this is expected to be called in sync with graph->clear(), which invalidates
  // cachedShortW_ and cachedShortb_ in the graph's short-term cache
  void clear() {
    shortlist_ = nullptr;
    cachedShortW_ = nullptr;
    cachedShortb_ = nullptr;
  }

  Logits apply(Expr input) override;

  virtual Logits apply(const std::vector<Expr>& /*inputs*/) override {
    ABORT("Not implemented");
  };
};

}  // namespace mlp

class Embedding : public LayerBase, public IEmbeddingLayer {
  Expr E_;
  Ptr<EmbeddingFactorMapping> embeddingFactorMapping_;
  Expr multiRows(const std::vector<IndexType>& data) const;
public:
  Embedding(Ptr<ExpressionGraph> graph, Ptr<Options> options);

  std::tuple<Expr/*embeddings*/, Expr/*mask*/> apply(Ptr<data::SubBatch> subBatch) const override final;

  // special version used in decoding
  Expr apply(const std::vector<IndexType>& embIdx, int dimBatch, int dimBeam) const override final;
};

class ULREmbedding : public LayerBase, public IEmbeddingLayer {
  std::vector<Expr> ulrEmbeddings_; // @TODO: These could now better be written as 6 named class members
public:
  ULREmbedding(Ptr<ExpressionGraph> graph, Ptr<Options> options) : LayerBase(graph, options) {
    std::string name = "url_embed"; //opt<std::string>("prefix");
    int dimKeys = opt<int>("dimTgtVoc");
    int dimQueries = opt<int>("dimSrcVoc");
    int dimEmb = opt<int>("dimEmb");
    int dimUlrEmb =  opt<int>("dimUlrEmb"); // ULR mono embed size
    bool fixed = opt<bool>("fixed", false);
    NodeInitializer initFunc = inits::glorot_uniform;
    std::string queryFile = opt<std::string>("ulrQueryFile");
    std::string keyFile = opt<std::string>("ulrKeysFile");
    bool trainTrans = opt<bool>("ulrTrainTransform", false);
    if (!queryFile.empty() && !keyFile.empty()) {
      initFunc = inits::from_word2vec(queryFile, dimQueries, dimUlrEmb, false);
      name = "ulr_query";
      fixed = true;
      auto query_embed = graph_->param(name, { dimQueries, dimUlrEmb }, initFunc, fixed);
      ulrEmbeddings_.push_back(query_embed);
      // keys embeds
      initFunc = inits::from_word2vec(keyFile, dimKeys, dimUlrEmb, false);
      name = "ulr_keys";
      fixed = true;
      auto key_embed = graph_->param(name, { dimKeys, dimUlrEmb }, initFunc, fixed);
      ulrEmbeddings_.push_back(key_embed);
      // actual  trainable embedding
      initFunc = inits::glorot_uniform;
      name = "ulr_embed";
      fixed = false;
      auto ulr_embed = graph_->param(name, {dimKeys , dimEmb }, initFunc, fixed);  // note the reverse dim
      ulrEmbeddings_.push_back(ulr_embed);
      // init  trainable src embedding
      name = "ulr_src_embed";
      auto ulr_src_embed = graph_->param(name, { dimQueries, dimEmb }, initFunc, fixed);
      ulrEmbeddings_.push_back(ulr_src_embed);
      // ulr transformation matrix
      //initFunc = inits::eye(1.f); // identity matrix  - is it ok to init wiht identity or shall we make this to the fixed case only
      if (trainTrans) {
        initFunc = inits::glorot_uniform;
        fixed = false;
      }
      else
      {
        initFunc = inits::eye(); // identity matrix
        fixed = true;
      }
      name = "ulr_transform";
      auto ulrTransform = graph_->param(name, { dimUlrEmb, dimUlrEmb }, initFunc, fixed);
      ulrEmbeddings_.push_back(ulrTransform);

      initFunc = inits::from_value(1.f);  // TBD: we should read sharable flags here - 1 means all sharable - 0 means no universal embeddings - should be zero for top freq only
      fixed = true;
      name = "ulr_shared";
      auto share_embed = graph_->param(name, { dimQueries, 1 }, initFunc, fixed);
      ulrEmbeddings_.push_back(share_embed);
    }
  }

  std::tuple<Expr/*embeddings*/, Expr/*mask*/> apply(Ptr<data::SubBatch> subBatch) const override final {
    auto queryEmbed   = ulrEmbeddings_[0]; // Q : dimQueries*dimUlrEmb
    auto keyEmbed     = ulrEmbeddings_[1]; // K : dimKeys*dimUlrEmb
    auto uniEmbed     = ulrEmbeddings_[2]; // E : dimQueries*dimEmb
    auto srcEmbed     = ulrEmbeddings_[3]; // I : dimQueries*dimEmb
    auto ulrTransform = ulrEmbeddings_[4]; // A : dimUlrEmb *dimUlrEmb
    auto ulrSharable  = ulrEmbeddings_[5]; // alpha : dimQueries*1
    int dimBatch = (int)subBatch->batchSize();
    int dimEmb = uniEmbed->shape()[-1];
    int dimWords = (int)subBatch->batchWidth();
    // D = K.A.QT
    // dimm(K) = univ_tok_vocab*uni_embed_size
    // dim A = uni_embed_size*uni_embed_size
    // dim Q: uni_embed_size * total_merged_vocab_size
    // dim D = univ_tok_vocab * total_merged_vocab_size
    // note all above can be precombuted and serialized if A is not trainiable and during decoding (TBD)
    // here we need to handle the mini-batch
    // extract raws corresponding to Xs in this minibatch from Q
    auto queryEmbeddings = rows(queryEmbed, subBatch->data());
    auto srcEmbeddings = rows(srcEmbed, subBatch->data());   // extract trainable src embeddings
    auto alpha = rows(ulrSharable, subBatch->data());  // extract sharable flags
    auto qt = dot(queryEmbeddings, ulrTransform, false, false);  //A: transform embeddings based on similarity A :  dimUlrEmb*dimUlrEmb
    auto sqrtDim=std::sqrt((float)queryEmbeddings->shape()[-1]);
    qt = qt/sqrtDim;  // normalize accordin to embed size to avoid dot prodcut growing large in magnitude with larger embeds sizes
    auto z = dot(qt, keyEmbed, false, true);      // query-key similarity 
    float dropProb = this->options_->get<float>("ulr-dropout", 0.0f);  // default no dropout
    z = dropout(z, dropProb);
    float tau = this->options_->get<float>("ulr-softmax-temperature", 1.0f);  // default no temperature
    // temperature in softmax is to control randomness of predictions
    // high temperature Softmax outputs are more close to each other
    // low temperatures the softmax become more similar to  "hardmax" 
    auto weights = softmax(z / tau);  // assume default  is dim=-1, what about temprature? - scaler ??
    auto chosenEmbeddings = dot(weights, uniEmbed);  // AVERAGE 
    auto chosenEmbeddings_mix = srcEmbeddings + alpha * chosenEmbeddings;  // this should be elementwise  broadcast
    auto batchEmbeddings = reshape(chosenEmbeddings_mix, { dimWords, dimBatch, dimEmb });
    auto graph = ulrEmbeddings_.front()->graph();
    auto batchMask = graph->constant({ dimWords, dimBatch, 1 },
                                     inits::from_vector(subBatch->mask()));
    return std::make_tuple(batchEmbeddings, batchMask);
  }

  Expr apply(const std::vector<IndexType>& embIdx, int dimBatch, int dimBeam) const override final {
    embIdx; dimBatch; dimBeam;
    ABORT("not implemented"); // ULR cannot be used for decoding
  }
};
}  // namespace marian
