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
//! chompact is a proof of concept C++ garbage collector, showing that all of the
//! information necessary to traverse the object graph can be dynamically determined
//! at runtime using C++ template metaprogramming.

// TODO output object graph to DOT

#include <cassert>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <stack>
#include <vector>
#include <sys/mman.h>

#ifndef NDEBUG
#include <typeinfo>
#endif

// TODO: assert at runtime that this is actually the page size.
const size_t PageSize = 4096;

// Divide and round up
#define DIVU(N, D) (N + D - 1) / (D)

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
	static const size_t Size = ((PageSize - sizeof(Heap*)) * 8) / (ObjectSize * 8 + 1);

	static const size_t BitsPerWord = sizeof(uintptr_t) * 8;

private:
	char m_data[Size * ObjectSize];

	//! We keep track of the marked bits
	uintptr_t m_marked[DIVU(Size, BitsPerWord)];

	Heap* m_heap;

public:
	DataPage(Heap* heap) : m_heap(heap) { }

	void* operator new(size_t s)
	{
		assert(s <= PageSize);
		return mmap(0, PageSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	}

	static DataPage* dataPage(CollectedBase* _p)
	{
		uintptr_t p = reinterpret_cast<uintptr_t>(_p);
		return reinterpret_cast<DataPage*>(p & ~(PageSize - 1));
	}

	Heap* heap() const
	{
		return m_heap;
	}

	void* pointer(size_t i)
	{
		return static_cast<void*>(&m_data[i * ObjectSize]);
	}

	void mark(size_t i)
	{
		m_marked[i / BitsPerWord] |= 1 << (i % BitsPerWord);
	}

	bool marked(size_t i) const
	{
		return m_marked[i / BitsPerWord] & 1 << (i % BitsPerWord);
	}

	void clear()
	{
		memset(m_marked, '\0', DIVU(Size, BitsPerWord));
		mark(Size - 1);
	}
};

//! To accomodate more advanced garbage collectors that will move objects, handles
//! are references to indirect pointers, rather than directly to the objects themselves.
class IndirectPointerBase
{
public:
	static const size_t Size = sizeof(uintptr_t) * 8;
	union
	{
		struct
		{
			unsigned m_padding : Size - 1;
			unsigned m_valid : 1;
		};

		unsigned m_data;
	} u;

	void* operator new(size_t s, Heap* heap);
};


//! The IndirectPointerPage is an array of IndirectPointer's that manages the removed
//! items in a free list.
class IndirectPointerPage
{
public:
	static const size_t Size = PageSize / sizeof(IndirectPointerBase) - 2 * sizeof(size_t);

	size_t m_begin, m_freeList;
	IndirectPointerBase m_handles[Size];

	void* operator new(size_t s)
	{
		assert(s <= PageSize);
		return mmap(0, PageSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	}

	IndirectPointerPage()
		: m_begin(1)
		, m_freeList(0)
	{
	}

	void* allocateIndirectPointer()
	{
		if (m_freeList)
		{
			uintptr_t allocated = m_freeList;
			assert(m_handles[allocated].u.m_valid);

			m_freeList = m_handles[allocated].u.m_data;

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

template<typename Class>
class IndirectPointer : public IndirectPointerBase
{
public:
	IndirectPointer(Class* ptr)
	{
		u.m_data = reinterpret_cast<uintptr_t>(ptr);
	}

	Class* operator->() const { return u.m_valid ? u.m_data : 0; }
	Class& operator*() const { return u.m_valid ? u.m_data : 0; }
};

class Heap
{
public:
	Heap();

	bool marked(CollectedBase*);
	void mark(CollectedBase*);

	static Heap* heap(CollectedBase*);

	void collect();
	void markChildren(CollectedBase* p);

	void* allocateObject(size_t size);
	void* allocateIndirectPointer();

private:
	std::stack<CollectedBase*> m_marking;

	std::vector<DataPage*> m_dataPages;
	size_t m_nextFreeDataPage;
	size_t m_nextFreeObject;

	std::vector<IndirectPointerPage*> m_indirectPointerPages;
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
	Handle() : m_iptr(0) {}
	Handle(Collected<Class>* ptr);

	Class& operator*() const { return m_iptr ? *reinterpret_cast<Class*>(m_iptr->u.m_data) : NULL; }
	Class* operator->() const { return m_iptr ? reinterpret_cast<Class*>(m_iptr->u.m_data) : NULL; }
	operator bool() const { return m_iptr ? m_iptr->u.m_data : 0; }

	Handle& operator=(Collected<Class>* collected)
	{
		if (m_iptr)
			m_iptr->u.m_data = collected;
		else
			m_iptr = new (Heap::heap(collected)) IndirectPointer<Class>(&collected->instance);
		return *this;
	}

	template<typename T>
	Handle& operator=(const Member<T, Class>& handle)
	{
		Collected<Class>* ptr = static_cast<Collected<Class>*>(handle.MemberBase<T>::m_ptr);
		if (m_iptr)
			m_iptr->u.m_data = reinterpret_cast<uintptr_t>(ptr);
		else
			m_iptr = new (Heap::heap(ptr)) IndirectPointer<Class>(&ptr->instance);
		return *this;
	}

	Handle& operator=(const Handle<Class>& handle)
	{
		Collected<Class>* ptr = reinterpret_cast<Collected<Class>*>(handle.m_iptr);
		if (m_iptr)
			m_iptr->u.m_data = reinterpret_cast<uintptr_t>(ptr);
		else
			m_iptr = new (Heap::heap(ptr)) IndirectPointer<Class>(&ptr->instance);
		return *this;
	}

private:
	IndirectPointer<Class>* m_iptr;
};

template<typename Class>
ObjectInfo<Class>::ObjectInfo()
	: m_numChildren(0)
	, m_initializing(true)
{
	Class c;
	for(size_t i = 0; i < m_numChildren; ++i)
		m_children[i] -= reinterpret_cast<uintptr_t>(&c);
#ifndef NDEBUG
	std::cout << "resolving: " << typeid(*this).name() << std::endl;
	for(size_t i = 0; i < m_numChildren; ++i)
		std::cout << "child: " << m_children[i] << std::endl;
	std::cout << std::string('*',20) << std::endl;
#endif
	m_finalized = true;
}


template<typename Class>
Handle<Class>::Handle(Collected<Class>* ptr)
	: m_iptr(new (Heap::heap(ptr)) IndirectPointer<Class>(&ptr->instance))
{
	// XXX why does this break without this line
	new (Heap::heap(ptr)) IndirectPointer<Class>(&ptr->instance);
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
	void* o = heap.allocateObject(size);
	return o;
}

Heap::Heap()
	: m_nextFreeDataPage(0)
	, m_nextFreeObject(0)
	, m_nextFreeIndirectPointerPage(0)
{
	m_dataPages.push_back(new DataPage(this));
	m_dataPages.back()->clear();

	m_indirectPointerPages.push_back(new IndirectPointerPage);
}

bool Heap::marked(CollectedBase* _p)
{
	uintptr_t p = reinterpret_cast<uintptr_t>(_p);
	return DataPage::dataPage(_p)->marked(p % DataPage::Size);
}

void Heap::mark(CollectedBase* _p)
{
	uintptr_t p = reinterpret_cast<uintptr_t>(_p);
	DataPage::dataPage(_p)->mark(p % DataPage::Size);
}

Heap* Heap::heap(CollectedBase* _p)
{
	uintptr_t p = reinterpret_cast<uintptr_t>(_p);
	return DataPage::dataPage(_p)->heap();
}

void Heap::collect()
{
	assert(m_marking.empty());

	// mark roots
	for (size_t i = 0; i < m_indirectPointerPages.size(); ++i)
	{
		for (size_t j = 0; j < IndirectPointerPage::Size; ++j)
		{
			CollectedBase* p = reinterpret_cast<CollectedBase*>(m_indirectPointerPages[i]->m_handles[i].u.m_data);
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

	// XXX increase the number of data pages

	collect();
}

void* Heap::allocateIndirectPointer()
{
	void* iptr;
	IndirectPointerPage* page;
	do
	{
		page = m_indirectPointerPages[m_nextFreeIndirectPointerPage]; 
		iptr = page->allocateIndirectPointer();

		if (iptr)
			return iptr;

		++m_nextFreeIndirectPointerPage;
	} while(m_nextFreeIndirectPointerPage != m_indirectPointerPages.size());
}

void* IndirectPointerBase::operator new(size_t s, Heap* heap)
{
	assert(s == sizeof(uintptr_t));
	heap->allocateIndirectPointer();
}


struct List
{
	int data;
	//! The first member is the type of the struct
	//! The second member is the type of the pointer
	Member<List, List> next;
};

int main()
{
	Heap heap;
	Handle<List> head = new(heap) Collected<List>;
	head->data = 0;

	Handle<List> list = head;
	for (int i = 1; i < 10; ++i)
	{
		Collected<List>* p = new(heap) Collected<List>;
		list->next = p;
		list = list->next;
		list->data = i;
	}

	// heap.collect();

	for (list = head; list; list = list->next)
	{
		std::cout << list->data << std::endl;
	}
}

