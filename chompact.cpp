// Copyright (c) 2010, Nathan Lawrence <ch@osga.me>
// All rights reserved.
// 
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//     * Redistributions of source code must retain the above copyright
//       notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above copyright
//       notice, this list of conditions and the following disclaimer in the
//       documentation and/or other materials provided with the distribution.
//     * Neither the name of the <organization> nor the
//       names of its contributors may be used to endorse or promote products
//       derived from this software without specific prior written permission.
// 
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
// ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
// WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL <COPYRIGHT HOLDER> BE LIABLE FOR ANY
// DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
// (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
// LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
// ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
// SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//
//! ComPPactor is a proof of concept C++ garbage collector, showing that all of the
//! information necessary to traverse the object graph can be dynamically determined
//! at runtime using C++ template metaprogramming.

// TODO output object graph to DOT

#include <cstdlib>
#include <iostream>
#include <vector>
#include <stack>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cassert>

// TODO: assert at runtime that this is actually the page size.
const size_t PageSize = 4096;

// Divide and round up
#define DIVU(N, D) (N + D - 1) / D

class Heap;
class CollectedBase;
template<typename Class> class Collected;

//! All heap objects are allocated on a DataPage.  DataPages are convenient, because since they are aligned
//! the static information on the data page can be accessed by any pointer allocated within the DataPage
//! without any additional space overhead.
//
// TODO: allign data pages
class DataPage
{
public:
	static const size_t ObjectSize = 0x10;

	//! This is the number of objects that we can keep in a single page, including the bits to mark the
	//! free spaces.
	static const size_t Size = (PageSize / ObjectSize * 8 / 9);

private:
	char* m_data[Size * ObjectSize];

	//! We keep track of the marked bits
	uintptr_t m_marked[DIVU(Size, sizeof(uintptr_t))];

public:
	void* pointer(size_t i)
	{
		return static_cast<void*>(&m_data[i * ObjectSize]);
	}

	void mark(size_t i)
	{
		m_marked[i / (sizeof(uintptr_t) * 8)] |= 1 << (i % (sizeof(uintptr_t) * 8));
	}

	bool marked(size_t i)
	{
		return m_marked[i / (sizeof(uintptr_t) * 8)] & 1 << (i % (sizeof(uintptr_t) * 8));
	}

	void clear()
	{
		memset(m_marked, '\0', DIVU(Size, sizeof(uintptr_t)));
		mark(Size - 1);
	}
};

//! To accomodate more advanced garbage collectors that will move objects, handles
//! are references to indirect pointers, rather than directly to the objects themselves.
struct IndirectPointer
{
	static const size_t Size = sizeof(uintptr_t) * 8;
	unsigned data : Size - 1;
	unsigned flag : 1;
};


//! The IndirectPointerPage is an array of IndirectPointer's that manages the removed
//! items in a free list.
struct IndirectPointerPage
{
	static const size_t Size = PageSize / sizeof(IndirectPointer) - 2 * sizeof(size_t);

	size_t m_begin, m_freeList;
	IndirectPointer m_handles[Size];

	IndirectPointerPage()
		: m_begin(1)
		, m_freeList(0)
	{
	}

	IndirectPointer* allocateIndirectPointer()
	{
		if (m_freeList)
		{
			uintptr_t allocated = m_freeList;
			assert(!m_handles[allocated].flag);

			m_freeList = m_handles[allocated].data;

			return &m_handles[allocated];
		}
		else if (m_begin <= Size)
		{
			return &m_handles[m_begin++];
		}
		else
		{
			return 0;
		}
	}
};

class Heap
{
public:
	Heap();

	bool marked(CollectedBase*);
	void mark(CollectedBase*);

	void collect();
	void* allocateObject(size_t size);
	void markChildren(CollectedBase* p);

	IndirectPointer* allocateIndirectPointer();

private:
	std::stack<CollectedBase*> m_marking;

	std::vector<DataPage*> m_dataPages;
	size_t m_nextFreeDataPage;
	size_t m_nextFreeObject;

	std::vector<IndirectPointerPage*> m_handlePages;
	size_t m_nextFreeIndirectPointerPage;
};

//! Member<> is a wrapper around the data members of a class used to automatically
//! generate the list of objects that need to be marked for each type.
template<typename Class>
class MemberBase
{
public:
	MemberBase();

	void* m_ptr;
};

template<typename Class>
class ObjectInfo
{
	friend class Collected<Class>;

public:
	ObjectInfo();
	bool finalized();
	void append(MemberBase<Class>*);

private:
	int m_numChildren;
	bool m_initializing, m_finalized;
	uintptr_t m_loader;
	uintptr_t m_children[sizeof(Class) / sizeof(MemberBase<Class>)];
};

class CollectedBase
{
public:

	virtual std::pair<CollectedBase**,CollectedBase**> children() = 0;
};

template<typename Class>
class Collected : public CollectedBase
{
public:
	Collected()
	{
		// ASSERT that this pointer has been
		// heap allocated
	}

	void* operator new(size_t size, Heap&);

	Class instance;
	static ObjectInfo<Class> info;

	std::pair<CollectedBase**,CollectedBase**> children()
	{
		return std::make_pair(
		          reinterpret_cast<CollectedBase**>(info.m_children),
		          reinterpret_cast<CollectedBase**>(info.m_children) + info.m_numChildren);
	}
};

template<typename Class>
class Handle;

template<typename Class, typename Property>
class Member : public MemberBase<Class>
{
public:
	Member() {}
	Member(Collected<Property>*);

	Property& operator*() const { return *reinterpret_cast<Property*>(MemberBase<Class>::m_ptr); }
	Property* operator->() const { return reinterpret_cast<Property*>(MemberBase<Class>::m_ptr); }
	operator bool() const { return MemberBase<Class>::m_ptr; }

	 Member& operator=(Collected<Property>* collected)
	{
		MemberBase<Class>::m_ptr = collected;
		return *this;
	}

	template<typename T>
	Member& operator=(const Member<T, Property>& handle)
	{
		MemberBase<Class>::m_ptr = handle.MemberBase<T>::m_ptr;
		return *this;
	}

	Member& operator=(const Handle<Property>& handle)
	{
		MemberBase<Class>::m_ptr = handle.m_ptr;
		return *this;
	}
};

template<typename Class>
class Handle
{
public:
	Handle() : m_ptr(0) {}
	Handle(Collected<Class>* ptr) : m_ptr(ptr) {}

	Class& operator*() const { return m_ptr->instance; }
	Class* operator->() const { return &m_ptr->instance; }
	operator bool() const { return m_ptr; }

	Handle& operator=(Collected<Class>* collected)
	{
		MemberBase<Class>::m_ptr = collected;
		return *this;
	}

	template<typename T>
	Handle& operator=(const Member<T, Class>& handle)
	{
		m_ptr = reinterpret_cast<Collected<Class>*>(handle.MemberBase<T>::m_ptr);
		return *this;
	}

	Handle& operator=(const Handle<Class>& handle)
	{
		m_ptr = handle.m_ptr;
		return *this;
	}

private:
	Collected<Class>* m_ptr;
};

template<typename Class>
ObjectInfo<Class>::ObjectInfo()
	: m_numChildren(0)
	, m_initializing(true)
{
	Class c;
	for(size_t i = 0; i < m_numChildren; ++i)
		m_children[i] -= reinterpret_cast<uintptr_t>(&c);
	for(size_t i = 0; i < m_numChildren; ++i)
		std::cout << "child: " << m_children[i] << std::endl;
	m_finalized = true;
}


Handle::Handle(Collected<Class>* ptr)
{
	Heap* heap = Heap::heap(ptr);
	m_ptr = heap->allocateHandle();
}

template<typename Class>
inline bool ObjectInfo<Class>::finalized()
{
	return m_finalized;
}

template<typename Class>
inline void ObjectInfo<Class>::append(MemberBase<Class>* child)
{
	if (!m_initializing)
		return;

	m_children[m_numChildren++] = reinterpret_cast<uintptr_t>(child);
}

template<typename Class>
MemberBase<Class>::MemberBase()
	: m_ptr(0)
{
	if (Collected<Class>::info.finalized())
		return;

	Collected<Class>::info.append(this);
}

template<typename Class>
ObjectInfo<Class> Collected<Class>::info;

template<typename Class>
void* Collected<Class>::operator new(size_t size, Heap& heap)
{
	return heap.allocateObject(size);
}

struct List
{
	int data;
	Member<List, List> next;
};

Heap::Heap()
	: m_nextFreeDataPage(0)
	, m_nextFreeObject(0)
	, m_nextFreeIndirectPointerPage(0)
{
	m_dataPages.push_back(new DataPage);
	m_dataPages.back()->clear();
}

bool Heap::marked(CollectedBase* _p)
{
	uintptr_t p = reinterpret_cast<uintptr_t>(_p);
	return m_dataPages[p / DataPage::Size]->marked(p % DataPage::Size);
}

void Heap::mark(CollectedBase* _p)
{
	uintptr_t p = reinterpret_cast<uintptr_t>(_p);
	m_dataPages[p / DataPage::Size]->mark(p % DataPage::Size);
}

void Heap::collect()
{
	assert(m_marking.empty());

	// mark roots
	for (size_t i = 0; i < m_handlePages.size(); ++i)
	{
		for (size_t j = 0; j < IndirectPointerPage::Size; ++j)
		{
			CollectedBase* p = reinterpret_cast<CollectedBase*>(m_handlePages[i]->m_handles[i].data & ((1U << 31U) - 1U));
			mark(p);
			markChildren(p);
		}
	}

	// mark children
	while (!m_marking.empty())
	{
		CollectedBase* p = m_marking.top();
		m_marking.pop();

		markChildren(p);
	}

	m_nextFreeDataPage = 0;
	m_nextFreeObject = 0;
}

void Heap::markChildren(CollectedBase* p)
{
	assert(marked(p));
		return;

	std::pair<CollectedBase**, CollectedBase**> children = p->children();
	CollectedBase** begin = children.first;
	CollectedBase** end = children.second;
	for (CollectedBase** q = begin; q != end; ++q)
	{
		if (marked(p))
			continue;
		mark(p);
		m_marking.push(*q);
	}
}

void* Heap::allocateObject(size_t size)
{
	DataPage* dp = m_dataPages[m_nextFreeDataPage];

	do
	{
		do
		{
			if (dp->marked(m_nextFreeObject))
			{
				dp->mark(m_nextFreeObject);
				void* object = dp->pointer(m_nextFreeObject);
				m_nextFreeObject++;

				return object;
			}
			++m_nextFreeObject;
		} while (m_nextFreeObject != DataPage::Size);
		m_nextFreeObject = 0;
		++m_nextFreeDataPage;
	} while (m_nextFreeDataPage != m_dataPages.size());

	// XXX add new things to m_dataPage

	collect();

	return malloc(size);
}

int main()
{
	Heap heap;
	Collected<List>* p = new(heap) Collected<List>;
	Handle<List> head = p;
	std::cout << 0 << "\t" << p << "\t" << &*head << std::endl;
	head->data = 0;

	Handle<List> list = head;
	for (int i = 1; i < 10; ++i)
	{
		Collected<List>* p = new(heap) Collected<List>;
		list->next = p;
		list = list->next;
		list->data = i;

		std::cout << i << "\t" << p << "\t" << &*list << std::endl;
	}

	std::cout << std::endl;

	heap.collect();

	for (list = head; list; list = list->next)
	{
		std::cout << list.operator->() << "\t" << list->data << std::endl;
	}
}

