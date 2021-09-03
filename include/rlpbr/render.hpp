#pragma once

#include <rlpbr/fwd.hpp>
#include <rlpbr/environment.hpp>
#include <memory>

namespace RLpbr {

struct BatchDeleter {
    void *state;
    void (*deletePtr)(void *, BatchBackend *);
    void operator()(BatchBackend *) const;
};

class RenderBatch {
public:
    using Handle = std::unique_ptr<BatchBackend, BatchDeleter>;

    RenderBatch(Handle &&backend,  std::vector<Environment> &&envs);

    inline void addEnvironment(Environment &&env)
    { 
        envs_.emplace_back(std::move(env));
    }

    inline Environment &getEnvironment(uint32_t idx) { return envs_[idx]; }
    inline Environment *getEnvironments() { return envs_.data(); }

    inline BatchBackend *getBackend() { return backend_.get(); }

private:
    Handle backend_;
    std::vector<Environment> envs_;
};

}
