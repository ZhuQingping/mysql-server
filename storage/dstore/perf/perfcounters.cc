/********************************************************************
 * Copyright (C) Huawei Technologies, 2018
 * Performance counters framework implementation:
 *  Contains counter catalog functions to keep track of all system counters
 ********************************************************************/

#include "perfcounters.h"

void printMock(int x, std::string log) {
  (void)x;
  (void)log;
};

using namespace std;

namespace Huawei {
namespace Common {

/*
 *  CounterCatalog::Get - returns instance (singleton of counter catalog)
 *
 * Parameters:
 *
 * Returns:
 * CounterCatalog global instance
 *
 */

CounterCatalog *CounterCatalog::Get() {
  static std::mutex init;
  static CounterCatalog *ccInstance = nullptr;
  if (ccInstance == nullptr) {
    std::unique_lock<std::mutex> lock(init);
    if (ccInstance == nullptr) {
      ccInstance = new CounterCatalog();
    }
  }

  return ccInstance;
}

CounterCatalog::CounterCatalog() : _nextID(0) {}

/*
 * Registers a counter catalog node with catalog
 *
 * Parameters:
 * h (IN)    - handle to the node that contains counters. Also handle is
 * pointer, it is used as a unique handle only and never is dereferenced. name
 * (IN)  - name of the node (user-defined string)
 */

void CounterCatalog::registerNode(const CounterNodeHandle h,
                                  const std::string &name) {
  lock_guard<mutex> lock(_mutex);
  auto it = _allNodes.find(h);
  CounterNodePtr cn;

  if (it != _allNodes.end()) {
    cn = it->second;
    if (cn->_name.length() > 0) {
      CDE_THROW(-1, "Counter already exists");
    }
    // node was registered without name, now we just change its name
    cn->_name = name;
  } else {
    cn = make_shared<CounterNode>(name);
    addToIdMap(cn.get(), _idNodeMap);
    CounterLibID cnId = cn->getId();
    CDE_ASSERT(cnId != InvalidCounterLibId);
    _allNodes[h] = cn;
    _rootNodes[cnId] = cn;
  }
}

/*
 * Removes a node with counters and child nodes from catalog.
 *
 *  h (IN) - handle to a class that was used during registration
 */

void CounterCatalog::unregisterNode(const CounterNodeHandle h) {
  lock_guard<mutex> lock(_mutex);
  auto it = _allNodes.find(h);
  if (it == _allNodes.end()) {
    // Node is not registered - it may be removed during its parent removal
    return;
  }
  CounterNodePtr node = it->second;
  _allNodes.erase(it);
  CounterLibID nodeId = node->getId();
  CDE_ASSERT(nodeId != InvalidCounterLibId);
  // removing from id index
  removeFromIdMap(node.get(), _idNodeMap);
  // Removing node from global and parent collections
  if (node->_parent == NullNodeHandle) {
    // Root node
    auto it1 = _rootNodes.find(nodeId);
    if (it1 != _rootNodes.end()) {
      _rootNodes.erase(it1);
    } else {
      CDE_THROW(-1, "Inconsistency, node without parent is not in root");
    }
  } else {
    // Child node
    CounterNodePtr pNode = getNode(node->_parent, false);
    auto it1 = pNode->_children.find(nodeId);
    if (it1 != pNode->_children.end()) {
      pNode->_children.erase(it1);
    } else {
      CDE_THROW(-1, "Inconsistency, parent doesn't have this child");
    }
  }
  // Removing children
  unregisterChildNodes(node);
}

/*
 *    UnregisterChildNodes - recursively removes child nodes from
 *                           of all nodes collection.
 *
 *    Parameters:
 *       node (IN) - node which children should be removed
 */

void CounterCatalog::unregisterChildNodes(const CounterNodePtr &node) {
  for (auto it : node->_counters) {
    removeFromIdMap(it, _idCounterMap);
  }
  for (auto it : node->_children) {
    CounterNodeHandle h = it.second->getHandle();
    CDE_ASSERT(h != NullNodeHandle);
    auto it1 = _allNodes.find(h);
    if (it1 == _allNodes.end()) {
      CDE_THROW(-1, "Inconsistency, child node does is not in allNodes");
    }
    removeFromIdMap(it1->second.get(), _idNodeMap);
    _allNodes.erase(it1);
    unregisterChildNodes(it.second);
  }
}

/*
 * Registers relation between two classes that contain counters
 * parent class has to be registered using RegisterClass()
 * Child class may not be registered yet, in this case the node fo child is
 * created
 *
 * child  (IN)   - handle (pointer) to a child class
 * parent (IN)  - handle (pointer) to a parent class
 */

void CounterCatalog::registerChild(const CounterNodeHandle child,
                                   const CounterNodeHandle parent) {
  lock_guard<mutex> lock(_mutex);
  CounterNodePtr pNode = getNode(parent, false);
  CounterNodePtr cNode = getNode(child, true);

  if (cNode->_parent != NullNodeHandle) {
    CDE_THROW(-1, "Node already have parent");
  }
  CounterLibID cNodeId = cNode->getId();
  CDE_ASSERT(cNodeId != InvalidCounterLibId);
  // By default node is added to the root collection
  auto it = _rootNodes.find(cNodeId);
  if (it != _rootNodes.end()) {
    _rootNodes.erase(it);
  } else {
    CDE_THROW(-1, "Inconsistency, cannot find child node in root nodes");
  }

  cNode->_parent = parent;
  cNode->_self = child;
  pNode->_children[cNodeId] = cNode;
}

/*
 * Adds a counter to a node
 * h        - handle to a node
 * counter  - counter to register
 */

void CounterCatalog::registerCounter(const CounterNodeHandle h,
                                     CounterBase *counter) {
  lock_guard<mutex> lock(_mutex);
  CounterNodePtr n = getNode(h, true);
  n->_counters.insert(counter);
  addToIdMap(counter, _idCounterMap);
}

/*
 * Removes counter registration
 * h        - handle to a node
 * counter  - counter to unregister
 */

void CounterCatalog::unregisterCounter(const CounterNodeHandle h,
                                       CounterBase *counter) {
  lock_guard<mutex> lock(_mutex);
  // Finding node
  auto it = _allNodes.find(h);
  if (it == _allNodes.end()) {
    return;
  }
  CounterNodePtr node = it->second;
  // finding counter
  auto it1 = node->_counters.find(counter);
  if (it1 == node->_counters.end()) {
    CDE_THROW(-1, "Counter was not registered");
  }
  node->_counters.erase(it1);
  removeFromIdMap(counter, _idCounterMap);
}

/*
 * Adds a catalog item to the map that maps ids to counters
 *
 * Parameters:
 * ci (IN) catalog item (counter or node)
 * map (IN) - map of ids to items
 *
 */
void CounterCatalog::addToIdMap(CounterCatalogItem *ci, CatalogIDMap &map) {
  CDE_ASSERT(ci->_id == InvalidCounterLibId);
  ci->_id = _nextID++;
  CDE_ASSERT(map.find(ci->_id) == map.end());
  map[ci->_id] = ci;
}

/*
 * removes catalog item from IdMap
 *
 * Parameters:
 * ci (IN) catalog item (counter or node)
 * map (IN) - map of ids to items
 */
void CounterCatalog::removeFromIdMap(CounterCatalogItem *ci,
                                     CatalogIDMap &map) {
  // finding counter in id map
  CDE_ASSERT(ci->_id != InvalidCounterLibId);
  auto it = map.find(ci->_id);
  if (it == map.end()) {
    CDE_THROW(-1, "Cannot find catalog item to remove from map");
  }
  map.erase(it);
  ci->_id = InvalidCounterLibId;
}

/*
 * GetNode  -  returns a node for given node handle.
 *
 * Parameter:
 *    h     (IN)   -  handle for node to return
 *    create(IN)   -  if node does not exist and create is true, node with given
 *                    handle will be created. Otherwise and exception will be
 *                    thrown.
 */
CounterNodePtr CounterCatalog::getNode(const CounterNodeHandle h, bool create) {
  CounterNodePtr ptr;
  auto it = _allNodes.find(h);
  if (it == _allNodes.end()) {
    if (create) {
      ptr = make_shared<CounterNode>("");
      _allNodes[h] = ptr;
      addToIdMap(ptr.get(), _idNodeMap);
      CounterLibID cid = ptr->getId();
      CDE_ASSERT(cid != InvalidCounterLibId);
      _rootNodes[cid] = ptr;
    } else {
      CDE_THROW(-1, "Node not found");
    }
  } else {
    ptr = it->second;
  }

  return ptr;
}

/*
 * getItem  -  returns a counter catalog item for given ID.
 *
 * Parameter:
 *    cid (IN)   -  ID for counter catalog item to return
 */
CounterCatalogItem *CounterCatalog::getItem(CounterLibID cid) const {
  CounterCatalogItem *ptr = nullptr;
  // finding counter in id map
  CDE_ASSERT(cid != InvalidCounterLibId);
  auto it = _idCounterMap.find(cid);
  if (it != _idCounterMap.end()) {
    ptr = it->second;
  }
  if (ptr == nullptr) {
    it = _idNodeMap.find(cid);
    if (it != _idNodeMap.end()) {
      ptr = it->second;
    }
  }
  return ptr;
}

/*
 * getItem  -  returns a counter catalog item for given name.
 *
 * Parameter:
 *    name (IN)   -  name for counter catalog item to return
 */
CounterCatalogItem *CounterCatalog::getItem(const std::string &name) const {
  CounterCatalogItem *ptr = nullptr;
  // finding counter in id map
  for (auto it = _idCounterMap.begin(); it != _idCounterMap.end(); ++it) {
    if (it->second->getName() == name) {
      ptr = it->second;
      break;
    }
  }
  if (ptr == nullptr) {
    for (auto it = _idNodeMap.begin(); it != _idNodeMap.end(); ++it) {
      if (it->second->getName() == name) {
        ptr = it->second;
        break;
      }
    }
  }
  return ptr;
}

/*
 * Prints all counters to a stream
 *
 * os     - stream where counters will be put.
 * filter - filter to select items to dump.
 */

void CounterCatalog::dump(ostream &os, PublisherFilter *filter) const {
  lock_guard<mutex> lock(_mutex);
  if (filter == nullptr || filter->op == PublisherFilterOperation::Unequal) {
    // dump without filter
    for (auto &it : _rootNodes) {
      dump(it.second.get(), os, nullptr, filter);
    }
  } else {
    // dump with filter
    std::vector<CounterCatalogItem *> items = processFilter(filter);
    for (auto it = items.begin(); it != items.end(); it++) {
      CounterBase *counter = dynamic_cast<CounterBase *>(*it);
      if (counter != nullptr) {
        os << *counter;
      } else {
        CounterNode *node = dynamic_cast<CounterNode *>(*it);
        if (node != nullptr) {
          dump(node, os, nullptr, filter);
        }
      }
    }
  }
}

/*
 * Prints all counters of node to a stream
 *
 * os     - stream where counters will be put.
 * filter - filter to select node to dump.
 */

void CounterCatalog::dumpRootNode(ostream &os, PublisherFilter *filter) const {
  lock_guard<mutex> lock(_mutex);
  if (filter == nullptr || filter->op == PublisherFilterOperation::Unequal) {
    // dump without filter
    for (auto &it : _rootNodes) {
      dump(it.second.get(), os, nullptr, filter);
    }
  } else {
    std::vector<CounterCatalogItem *> items;
    std::string filter_value(filter->value);
    std::vector<std::string> values = tokenSplit(filter_value, ',');

    for (const std::string &value : values) {
      for (auto &rootNode : _rootNodes) {
        if (rootNode.second->getName() == value) {
          dump(rootNode.second.get(), os, nullptr, filter);
        }
      }
    }
  }
}

/*
 * processFilter  -  returns a list of counter catalog items based on filter
 *
 * Parameter:
 *    filter (IN) -  filter for counter catalog items
 */
std::vector<CounterCatalogItem *> CounterCatalog::processFilter(
    PublisherFilter *filter) const {
  std::vector<CounterCatalogItem *> result;
  std::string filter_value(filter->value);
  std::vector<std::string> values = tokenSplit(filter_value, ',');

  for (const std::string &value : values) {
    CounterCatalogItem *item = nullptr;
    switch (filter->field) {
      case PublisherFilterField::Id:
        try {
          CounterLibID cid = std::stoull(value);
          item = getItem(cid);
        } catch (const std::invalid_argument &e) {
          // nothing
        }
        break;
      case PublisherFilterField::Name:
        item = getItem(value);
        break;
      default:
        break;
    }
    if (item != nullptr) {
      result.push_back(item);
    }
  }
  return result;
}

/*
 * reset  -  support all counter reset and special counter reset
 *
 * Parameter:
 *    filter (IN) -  filter for counter reset items
 */
void CounterCatalog::reset(PublisherFilter *filter) {
  lock_guard<mutex> lock(_mutex);
  if (filter == nullptr || filter->op == PublisherFilterOperation::Unequal) {
    for (auto it : _rootNodes) {
      reset(it.second);
    }
    return;
  }
  // reset special counter items
  std::string filterValue(filter->value);
  std::vector<std::string> values = tokenSplit(filterValue, ',');
  for (const std::string &value : values) {
    for (auto it : _allNodes) {
      try {
        if ((filter->field == PublisherFilterField::Name &&
             it.second->getName() == value) ||
            (filter->field == PublisherFilterField::Id &&
             it.second->getId() == std::stoull(value))) {
          reset(it.second);
        }
      } catch (std::exception &e) {
        std::cerr << "Reset and Filter option exception: " << e.what()
                  << std::endl;
      }
    }
  }
}

/*
 * Prints all counters IDs to a stream.
 *
 * os     - output stream to which counters will be print
 * filter - filter to select counters to print
 */
void CounterCatalog::listCounterIds(ostream &os,
                                    PublisherFilter *filter) const {
  lock_guard<mutex> lock(_mutex);
  if (filter == nullptr || filter->op == PublisherFilterOperation::Unequal) {
    // list IDs without filter
    for (auto it : _rootNodes) {
      listCounterIds(it.second.get(), os, "", filter);
    }
  } else {
    // list IDs with filter
    std::vector<CounterCatalogItem *> items = processFilter(filter);
    for (auto it = items.begin(); it != items.end(); it++) {
      CounterBase *counter = dynamic_cast<CounterBase *>(*it);
      if (counter != nullptr) {
        os << counter->getName() << ": " << counter->getId() << endl;
      } else {
        CounterNode *node = dynamic_cast<CounterNode *>(*it);
        if (node != nullptr) {
          listCounterIds(node, os, "", filter);
        }
      }
    }
  }
}

/*
 * Prints all counters of a node to a stream as well as counters that
 * belong to child nodes.
 *
 * node   - node to print counters from
 * os     - output stream to which counters will be print
 * parent - the parent node
 */
void CounterCatalog::dump(CounterNode *node, ostream &os, CounterNode *parent,
                          PublisherFilter *filter) const {
  if (filter != nullptr) {
    std::string filter_value(filter->value);
    std::vector<std::string> values = tokenSplit(filter_value, ',');
    std::string node_flag;
    if (filter->field == PublisherFilterField::Id) {
      node_flag = node->getId();
    } else {
      node_flag = node->getName();
    }
    if (filter->op == PublisherFilterOperation::Unequal) {
      if (std::find(values.begin(), values.end(), node_flag) != values.end()) {
        return;
      }
    }
  }

  node->dump(os, parent);
  for (auto it : node->_counters) {
    it->dump(os, parent);
  }
  for (auto it : node->_children) {
    dump(it.second.get(), os, node, filter);
  }
}

/*
 * Resets all counters of a specific node as well as counters of child nodes
 *
 * node  - node to print counters from
 */

void CounterCatalog::reset(const CounterNodePtr &node) {
  for (auto it : node->_counters) {
    it->reset();
  }
  for (auto it : node->_children) {
    reset(it.second);
  }
}

/*
 * Prints all counter ids of a node to a stream as well as counters that
 * belong to child nodes.
 *
 * node  - node to print counters from
 * os    - output stream to which counters will be print
 * depth - distance from the root node
 */
void CounterCatalog::listCounterIds(CounterNode *node, ostream &os,
                                    const std::string &prefix,
                                    PublisherFilter *filter) const {
  if (filter != nullptr) {
    std::string filter_value(filter->value);
    std::vector<std::string> values = tokenSplit(filter_value, ',');
    std::string node_flag;
    if (filter->field == PublisherFilterField::Id) {
      node_flag = node->getId();
    } else {
      node_flag = node->getName();
    }
    if (filter->op == PublisherFilterOperation::Unequal) {
      if (std::find(values.begin(), values.end(), node_flag) != values.end()) {
        return;
      }
    }
  }
  for (auto it : node->_counters) {
    os << prefix << node->_name << "." << it->getName() << ": " << it->getId()
       << endl;
  }
  for (auto it : node->_children) {
    listCounterIds(it.second.get(), os, prefix + node->_name + ".", filter);
  }
}

}  // namespace Common
}  // namespace Huawei
