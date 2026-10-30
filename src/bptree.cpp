#include "minidb.hpp"
#include <algorithm>
#include <stdexcept>

namespace minidb {
namespace { constexpr std::size_t MAX_KEYS = 8; }

struct BPlusTree::Node {
    explicit Node(bool leaf) : leaf(leaf) {}
    bool leaf;
    std::vector<std::int64_t> keys;
    std::vector<std::size_t> values;
    std::vector<std::unique_ptr<Node>> children;
    Node* next = nullptr;
};
struct BPlusTree::Split {
    std::int64_t key;
    std::unique_ptr<Node> right;
};

BPlusTree::BPlusTree() : root_(std::make_unique<Node>(true)) {}
BPlusTree::BPlusTree(BPlusTree&&) noexcept = default;
BPlusTree& BPlusTree::operator=(BPlusTree&&) noexcept = default;
BPlusTree::~BPlusTree() = default;

std::optional<BPlusTree::Split> BPlusTree::insert(Node& node, std::int64_t key, std::size_t value) {
    if (node.leaf) {
        auto it = std::lower_bound(node.keys.begin(), node.keys.end(), key);
        const auto position = static_cast<std::size_t>(it - node.keys.begin());
        if (it != node.keys.end() && *it == key) {
            node.values[position] = value;
            return std::nullopt;
        }
        node.keys.insert(it, key);
        node.values.insert(node.values.begin() + static_cast<std::ptrdiff_t>(position), value);
        ++size_;
        if (node.keys.size() <= MAX_KEYS) return std::nullopt;
        auto right = std::make_unique<Node>(true);
        const auto mid = node.keys.size() / 2;
        right->keys.assign(node.keys.begin() + static_cast<std::ptrdiff_t>(mid), node.keys.end());
        right->values.assign(node.values.begin() + static_cast<std::ptrdiff_t>(mid), node.values.end());
        node.keys.resize(mid);
        node.values.resize(mid);
        right->next = node.next;
        node.next = right.get();
        return Split{right->keys.front(), std::move(right)};
    }
    const auto it = std::upper_bound(node.keys.begin(), node.keys.end(), key);
    const auto index = static_cast<std::size_t>(it - node.keys.begin());
    auto split = insert(*node.children[index], key, value);
    if (!split) return std::nullopt;
    node.keys.insert(node.keys.begin() + static_cast<std::ptrdiff_t>(index), split->key);
    node.children.insert(node.children.begin() + static_cast<std::ptrdiff_t>(index + 1), std::move(split->right));
    if (node.keys.size() <= MAX_KEYS) return std::nullopt;
    const auto mid = node.keys.size() / 2;
    auto right = std::make_unique<Node>(false);
    const auto promoted = node.keys[mid];
    right->keys.assign(node.keys.begin() + static_cast<std::ptrdiff_t>(mid + 1), node.keys.end());
    for (auto child = node.children.begin() + static_cast<std::ptrdiff_t>(mid + 1); child != node.children.end(); ++child)
        right->children.push_back(std::move(*child));
    node.keys.resize(mid);
    node.children.resize(mid + 1);
    return Split{promoted, std::move(right)};
}

void BPlusTree::upsert(std::int64_t key, std::size_t value) {
    auto split = insert(*root_, key, value);
    if (split) {
        auto parent = std::make_unique<Node>(false);
        parent->keys.push_back(split->key);
        parent->children.push_back(std::move(root_));
        parent->children.push_back(std::move(split->right));
        root_ = std::move(parent);
    }
}

std::optional<std::size_t> BPlusTree::find(std::int64_t key) const {
    const Node* node = root_.get();
    while (!node->leaf) {
        const auto it = std::upper_bound(node->keys.begin(), node->keys.end(), key);
        node = node->children[static_cast<std::size_t>(it - node->keys.begin())].get();
    }
    const auto it = std::lower_bound(node->keys.begin(), node->keys.end(), key);
    if (it == node->keys.end() || *it != key) return std::nullopt;
    return node->values[static_cast<std::size_t>(it - node->keys.begin())];
}

std::vector<std::pair<std::int64_t, std::size_t>> BPlusTree::entries() const {
    const Node* node = root_.get();
    while (!node->leaf) node = node->children.front().get();
    std::vector<std::pair<std::int64_t, std::size_t>> result;
    result.reserve(size_);
    while (node) {
        for (std::size_t i = 0; i < node->keys.size(); ++i)
            result.emplace_back(node->keys[i], node->values[i]);
        node = node->next;
    }
    return result;
}
} // namespace minidb
