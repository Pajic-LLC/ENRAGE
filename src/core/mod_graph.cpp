#include "core/mod_graph.hpp"

#include <algorithm>
#include <cstddef>
#include <queue>
#include <unordered_map>
#include <unordered_set>

namespace enrage::core {

bool ModResolution::ok() const noexcept {
    return issues.empty();
}

ModResolution ModGraph::resolve(std::span<const ModSpec> mods) {
    ModResolution result;
    std::unordered_map<std::string, std::size_t> by_id;
    by_id.reserve(mods.size());

    for (std::size_t index = 0; index < mods.size(); ++index) {
        const auto& id = mods[index].id;
        if (id.empty()) {
            result.issues.push_back({ModIssueKind::duplicate_id, id, {}});
            continue;
        }

        const auto [_, inserted] = by_id.emplace(id, index);
        if (!inserted) {
            result.issues.push_back({ModIssueKind::duplicate_id, id, id});
        }
    }

    if (by_id.size() != mods.size()) {
        return result;
    }

    std::vector<std::vector<std::size_t>> edges(mods.size());
    std::vector<std::size_t> indegree(mods.size());

    const auto add_edge = [&](std::size_t from, std::size_t to) {
        auto& outgoing = edges[from];
        if (std::ranges::find(outgoing, to) != outgoing.end()) {
            return;
        }
        outgoing.push_back(to);
        ++indegree[to];
    };

    std::unordered_set<std::string> reported_conflicts;

    for (std::size_t index = 0; index < mods.size(); ++index) {
        const auto& mod = mods[index];

        for (const auto& dependency : mod.dependencies) {
            const auto found = by_id.find(dependency);
            if (found == by_id.end()) {
                result.issues.push_back({ModIssueKind::missing_dependency, mod.id, dependency});
                continue;
            }
            add_edge(found->second, index);
        }

        for (const auto& after : mod.load_after) {
            const auto found = by_id.find(after);
            if (found != by_id.end()) {
                add_edge(found->second, index);
            }
        }

        for (const auto& conflict : mod.conflicts) {
            if (!by_id.contains(conflict) || conflict == mod.id) {
                continue;
            }

            const auto first = std::min(mod.id, conflict);
            const auto second = std::max(mod.id, conflict);
            const auto key = first + "\n" + second;
            if (reported_conflicts.insert(key).second) {
                result.issues.push_back({ModIssueKind::conflict, first, second});
            }
        }
    }

    struct ReadyCompare {
        std::span<const ModSpec> mods;

        bool operator()(std::size_t left, std::size_t right) const {
            if (mods[left].priority != mods[right].priority) {
                return mods[left].priority > mods[right].priority;
            }
            return mods[left].id > mods[right].id;
        }
    };

    std::priority_queue<std::size_t, std::vector<std::size_t>, ReadyCompare> ready{
        ReadyCompare{mods}};

    for (std::size_t index = 0; index < indegree.size(); ++index) {
        if (indegree[index] == 0) {
            ready.push(index);
        }
    }

    while (!ready.empty()) {
        const auto index = ready.top();
        ready.pop();
        result.order.push_back(mods[index].id);

        for (const auto target : edges[index]) {
            if (--indegree[target] == 0) {
                ready.push(target);
            }
        }
    }

    if (result.order.size() != mods.size()) {
        for (std::size_t index = 0; index < indegree.size(); ++index) {
            if (indegree[index] != 0) {
                result.issues.push_back({ModIssueKind::cycle, mods[index].id, {}});
            }
        }
    }

    return result;
}

} // namespace enrage::core
