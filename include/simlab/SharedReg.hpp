#pragma once
// Copy-on-write handle for immutable-after-load content registries
// (properties, materials, deposits, processes, species, influx).
//
// World::clone()/clone_local() run thousands of times per autonomous cycle
// (speculative planning). Copying every registry by value — string maps,
// material property maps — dominated clone cost, and gave every clone a new
// PropertyRegistry::uid(), which invalidated all name->PropId caches each
// time planning switched between the real world and a clone. The handle
// shares ONE registry object between a world and all of its clones; the
// first mutation through a shared handle copies it first (copy-on-write),
// so a world can never see another world's edits.
//
// Call sites are unchanged: `w.props.find_name(...)`, `w.materials.id_of(...)`
// forward through the handle, and the handle converts to `const R&` for any
// function taking the registry by reference.
#include <memory>
#include <utility>

namespace simlab {

template <class R>
class SharedReg {
public:
    SharedReg() : p_(std::make_shared<R>()) {}
    SharedReg(const SharedReg&) = default;            // share, never copy content
    SharedReg(SharedReg&&) noexcept = default;
    SharedReg& operator=(const SharedReg&) = default;
    SharedReg& operator=(SharedReg&&) noexcept = default;
    SharedReg& operator=(R&& r) { p_ = std::make_shared<R>(std::move(r)); return *this; }
    SharedReg& operator=(const R& r) { p_ = std::make_shared<R>(r); return *this; }

    const R& get() const { return *p_; }
    operator const R&() const { return *p_; }
    // writable access: detaches from every other world first
    R& mut() {
        if (p_.use_count() > 1) p_ = std::make_shared<R>(*p_);
        return *p_;
    }
    bool shares_with(const SharedReg& o) const { return p_ == o.p_; }

    // ---- read API forwarding (whatever the registry type provides) ----------
    template <class... A, class Q = R> auto find(A&&... a) const
        -> decltype(std::declval<const Q&>().find(std::forward<A>(a)...)) {
        return p_->find(std::forward<A>(a)...);
    }
    template <class... A, class Q = R> auto find_name(A&&... a) const
        -> decltype(std::declval<const Q&>().find_name(std::forward<A>(a)...)) {
        return p_->find_name(std::forward<A>(a)...);
    }
    template <class... A, class Q = R> auto id_of(A&&... a) const
        -> decltype(std::declval<const Q&>().id_of(std::forward<A>(a)...)) {
        return p_->id_of(std::forward<A>(a)...);
    }
    template <class... A, class Q = R> auto value_ok(A&&... a) const
        -> decltype(std::declval<const Q&>().value_ok(std::forward<A>(a)...)) {
        return p_->value_ok(std::forward<A>(a)...);
    }
    template <class... A, class Q = R> auto unmet_dependencies(A&&... a) const
        -> decltype(std::declval<const Q&>().unmet_dependencies(std::forward<A>(a)...)) {
        return p_->unmet_dependencies(std::forward<A>(a)...);
    }
    template <class Q = R> auto defs() const -> decltype(std::declval<const Q&>().defs()) {
        return p_->defs();
    }
    template <class Q = R> auto size() const -> decltype(std::declval<const Q&>().size()) {
        return p_->size();
    }
    template <class Q = R> auto uid() const -> decltype(std::declval<const Q&>().uid()) {
        return p_->uid();
    }
    // ---- write API forwarding (copy-on-write) ------------------------------------
    template <class... A, class Q = R> auto add(A&&... a)
        -> decltype(std::declval<Q&>().add(std::forward<A>(a)...)) {
        return mut().add(std::forward<A>(a)...);
    }
    template <class... A, class Q = R> auto parse(A&&... a)
        -> decltype(std::declval<Q&>().parse(std::forward<A>(a)...)) {
        return mut().parse(std::forward<A>(a)...);
    }
    template <class... A, class Q = R> auto load_file(A&&... a)
        -> decltype(std::declval<Q&>().load_file(std::forward<A>(a)...)) {
        return mut().load_file(std::forward<A>(a)...);
    }

private:
    std::shared_ptr<R> p_;
};

}  // namespace simlab
