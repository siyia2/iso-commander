// SPDX-License-Identifier: GPL-3.0-or-later

import std;

// These functions have global-module linkage, as declared in
// isocmd.threadpool.threadpool's extern "C++" block. This TU deliberately
// needs only the standard library, not the queue and hazard-pointer modules.
namespace threadpool_detail {

    std::shared_ptr<std::promise<void>> preparePromise(std::future<void>& result) {
        auto promise = std::make_shared<std::promise<void>>();
        result = promise->get_future();
        return promise;
    }

    void completePromise(std::promise<void>& promise) {
        promise.set_value();
    }

    void failPromise(std::promise<void>& promise) {
        promise.set_exception(std::current_exception());
    }

} // namespace threadpool_detail
