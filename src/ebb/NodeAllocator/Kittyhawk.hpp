/*
  EbbRT: Distributed, Elastic, Runtime
  Copyright (C) 2013 SESA Group, Boston University

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU Affero General Public License as
  published by the Free Software Foundation, either version 3 of the
  License, or (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU Affero General Public License for more details.

  You should have received a copy of the GNU Affero General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/
#ifndef EBBRT_EBB_NODEALLOCATOR_KITTYHAWK_HPP
#define EBBRT_EBB_NODEALLOCATOR_KITTYHAWK_HPP

#include <unordered_map>

#include "ebb/NodeAllocator/NodeAllocator.hpp"

namespace ebbrt {
  class Kittyhawk : public NodeAllocator {
  public:
    static EbbRoot* ConstructRoot();
    Kittyhawk(EbbId id);
    unsigned int Allocate(std::string app, std::string config) override;
    void Free(unsigned int id) override;
  };
}

#endif
