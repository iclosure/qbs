/****************************************************************************
**
** Copyright (C) 2016 The Qt Company Ltd.
** Contact: https://www.qt.io/licensing/
**
** This file is part of Qbs.
**
** $QT_BEGIN_LICENSE:LGPL$
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see https://www.qt.io/terms-conditions. For further
** information use the contact form at https://www.qt.io/contact-us.
**
** GNU Lesser General Public License Usage
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 3 as published by the Free Software
** Foundation and appearing in the file LICENSE.LGPL3 included in the
** packaging of this file. Please review the following information to
** ensure the GNU Lesser General Public License version 3 requirements
** will be met: https://www.gnu.org/licenses/lgpl-3.0.html.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 2.0 or (at your option) the GNU General
** Public license version 3 or any later version approved by the KDE Free
** Qt Foundation. The licenses are as published by the Free Software
** Foundation and appearing in the file LICENSE.GPL2 and LICENSE.GPL3
** included in the packaging of this file. Please review the following
** information to ensure the GNU General Public License requirements will
** be met: https://www.gnu.org/licenses/gpl-2.0.html and
** https://www.gnu.org/licenses/gpl-3.0.html.
**
** $QT_END_LICENSE$
**
****************************************************************************/

#ifndef QBS_PERSISTENCE
#define QBS_PERSISTENCE

#include "error.h"
#include <logging/logger.h>

#include <QtCore/qdatastream.h>
#include <QtCore/qflags.h>
#include <QtCore/qprocess.h>
#include <QtCore/qregexp.h>
#include <QtCore/qstring.h>
#include <QtCore/qvariant.h>

#include <ctime>
#include <memory>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace qbs {
namespace Internal {

class NoBuildGraphError : public ErrorInfo
{
public:
    NoBuildGraphError(const QString &filePath);
};

class PersistentPool
{
public:
    PersistentPool(Logger &logger);
    ~PersistentPool();

    class HeadData
    {
    public:
        QVariantMap projectConfig;
    };

    // We need a helper class template, because we require partial specialization for some of
    // the aggregate types, which is not possible with function templates.
    // The generic implementation assumes that T is of class type and has load() and store()
    // member functions.
    template<typename T, typename Enable = void> struct Helper
    {
        static void store(const T &object, PersistentPool *pool) { const_cast<T &>(object).store(*pool); }
        static void load(T &object, PersistentPool *pool) { object.load(*pool); }
    };

    template<typename T, typename ...Types> void store(const T &value, const Types &...args)
    {
        Helper<T>().store(value, this);
        store(args...);
    }

    template<typename T, typename ...Types> void load(T &value, Types &...args)
    {
        Helper<T>().load(value, this);
        load(args...);
    }
    template<typename T> T load() {
        T tmp;
        Helper<T>().load(tmp, this);
        return tmp;
    }

    enum OpType { Store, Load };
    template<OpType type, typename T, typename ...Types> struct OpTypeHelper { };
    template<typename T, typename ...Types> struct OpTypeHelper<Store, T, Types...>
    {
        static void serializationOp(PersistentPool *pool, const T &value, const Types &...args)
        {
            pool->store(value, args...);
        }
    };
    template<typename T, typename ...Types> struct OpTypeHelper<Load, T, Types...>
    {
        static void serializationOp(PersistentPool *pool, T &value, Types &...args)
        {
            pool->load(value, args...);
        }
    };
    template<OpType type, typename T, typename ...Types> void serializationOp(const T &value,
                                                                              const Types &...args)
    {
        OpTypeHelper<type, T, Types...>::serializationOp(this, value, args...);
    }
    template<OpType type, typename T, typename ...Types> void serializationOp(T &value,
                                                                              Types &...args)
    {
        OpTypeHelper<type, T, Types...>::serializationOp(this, value, args...);
    }

    void load(const QString &filePath);
    void setupWriteStream(const QString &filePath);
    void finalizeWriteStream();
    void closeStream();
    void clear();

    const HeadData &headData() const { return m_headData; }
    void setHeadData(const HeadData &hd) { m_headData = hd; }

private:
    typedef int PersistentObjectId;

    template <typename T> T *idLoad();
    template <class T> std::shared_ptr<T> idLoadS();

    template<typename T> void storeSharedObject(const T *object);

    void storeVariant(const QVariant &variant);
    QVariant loadVariant();

    void storeString(const QString &t);
    QString loadString(int id);
    QString idLoadString();

    // Recursion termination
    void store() {}
    void load() {}

    QDataStream m_stream;
    HeadData m_headData;
    std::vector<void *> m_loadedRaw;
    std::vector<std::shared_ptr<void>> m_loaded;
    QHash<const void*, int> m_storageIndices;
    PersistentObjectId m_lastStoredObjectId;

    std::vector<QString> m_stringStorage;
    QHash<QString, int> m_inverseStringStorage;
    PersistentObjectId m_lastStoredStringId;
    Logger &m_logger;
};

template<typename T> inline const void *uniqueAddress(const T *t) { return t; }

template<typename T> inline void PersistentPool::storeSharedObject(const T *object)
{
    if (!object) {
        m_stream << -1;
        return;
    }
    const void * const addr = uniqueAddress(object);
    PersistentObjectId id = m_storageIndices.value(addr, -1);
    if (id < 0) {
        id = m_lastStoredObjectId++;
        m_storageIndices.insert(addr, id);
        m_stream << id;
        store(*object);
    } else {
        m_stream << id;
    }
}

template <typename T> inline T *PersistentPool::idLoad()
{
    PersistentObjectId id;
    m_stream >> id;

    if (id < 0)
        return nullptr;

    if (id < static_cast<PersistentObjectId>(m_loadedRaw.size()))
        return static_cast<T *>(m_loadedRaw.at(id));

    auto i = m_loadedRaw.size();
    m_loadedRaw.resize(id + 1);
    for (; i < m_loadedRaw.size(); ++i)
        m_loadedRaw[i] = nullptr;

    auto t = new T;
    m_loadedRaw[id] = t;
    load(*t);
    return t;
}

template <class T> inline std::shared_ptr<T> PersistentPool::idLoadS()
{
    PersistentObjectId id;
    m_stream >> id;

    if (id < 0)
        return std::shared_ptr<T>();

    if (id < static_cast<PersistentObjectId>(m_loaded.size()))
        return std::static_pointer_cast<T>(m_loaded.at(id));

    m_loaded.resize(id + 1);
    const std::shared_ptr<T> t = T::create();
    m_loaded[id] = t;
    load(*t);
    return t;
}

/***** Specializations of Helper class *****/

template<typename T>
struct PersistentPool::Helper<T, std::enable_if_t<std::is_member_function_pointer<
        decltype(&T::template completeSerializationOp<PersistentPool::Load>)>::value>>
{
    static void store(const T &value, PersistentPool *pool)
    {
        const_cast<T &>(value).template completeSerializationOp<PersistentPool::Store>(*pool);
    }
    static void load(T &value, PersistentPool *pool)
    {
        value.template completeSerializationOp<PersistentPool::Load>(*pool);
    }
};

template<typename T> struct PersistentPool::Helper<T, std::enable_if_t<std::is_integral<T>::value>>
{
    static void store(const T &value, PersistentPool *pool) { pool->m_stream << value; }
    static void load(T &value, PersistentPool *pool) { pool->m_stream >> value; }
};

template<> struct PersistentPool::Helper<long>
{
    static void store(long value, PersistentPool *pool) { pool->m_stream << qint64(value); }
    static void load(long &value, PersistentPool *pool)
    {
        qint64 v;
        pool->m_stream >> v;
        value = long(v);
    }
};

template<typename T>
struct PersistentPool::Helper<T, std::enable_if_t<std::is_same<T, std::time_t>::value
        && !std::is_same<T, long>::value>>
{
    static void store(std::time_t value, PersistentPool *pool) { pool->m_stream << qint64(value); }
    static void load(std::time_t &value, PersistentPool *pool)
    {
        qint64 v;
        pool->m_stream >> v;
        value = static_cast<std::time_t>(v);
    }
};

template<typename T> struct PersistentPool::Helper<T, std::enable_if_t<std::is_enum<T>::value>>
{
    using U = std::underlying_type_t<T>;
    static void store(const T &value, PersistentPool *pool)
    {
        pool->m_stream << static_cast<U>(value);
    }
    static void load(T &value, PersistentPool *pool)
    {
        pool->m_stream >> reinterpret_cast<U &>(value);
    }
};

template<typename T> struct PersistentPool::Helper<std::shared_ptr<T>>
{
    static void store(const std::shared_ptr<T> &value, PersistentPool *pool)
    {
        pool->store(value.get());
    }
    static void load(std::shared_ptr<T> &value, PersistentPool *pool)
    {
        value = pool->idLoadS<std::remove_const_t<T>>();
    }
};

template<typename T> struct PersistentPool::Helper<std::unique_ptr<T>>
{
    static void store(const std::unique_ptr<T> &value, PersistentPool *pool)
    {
        pool->store(value.get());
    }
    static void load(std::unique_ptr<T> &ptr, PersistentPool *pool)
    {
        ptr.reset(pool->idLoad<std::remove_const_t<T>>());
    }
};

template<typename T> struct PersistentPool::Helper<T *>
{
    static void store(const T *value, PersistentPool *pool) { pool->storeSharedObject(value); }
    void load(T* &value, PersistentPool *pool) { value = pool->idLoad<T>(); }
};

template<> struct PersistentPool::Helper<QString>
{
    static void store(const QString &s, PersistentPool *pool) { pool->storeString(s); }
    static void load(QString &s, PersistentPool *pool) { s = pool->idLoadString(); }
};

template<> struct PersistentPool::Helper<QVariant>
{
    static void store(const QVariant &v, PersistentPool *pool) { pool->storeVariant(v); }
    static void load(QVariant &v, PersistentPool *pool) { v = pool->loadVariant(); }
};

template<> struct PersistentPool::Helper<QRegExp>
{
    static void store(const QRegExp &re, PersistentPool *pool) { pool->store(re.pattern()); }
    static void load(QRegExp &re, PersistentPool *pool) { re.setPattern(pool->idLoadString()); }
};

template<> struct PersistentPool::Helper<QProcessEnvironment>
{
    static void store(const QProcessEnvironment &env, PersistentPool *pool)
    {
        const QStringList &keys = env.keys();
        pool->store(keys.size());
        for (const QString &key : keys) {
            pool->store(key);
            pool->store(env.value(key));
        }
    }
    static void load(QProcessEnvironment &env, PersistentPool *pool)
    {
        const int count = pool->load<int>();
        for (int i = 0; i < count; ++i) {
            const auto &key = pool->load<QString>();
            const auto &value = pool->load<QString>();
            env.insert(key, value);
        }
    }
};
template<typename T, typename U> struct PersistentPool::Helper<std::pair<T, U>>
{
    static void store(const std::pair<T, U> &pair, PersistentPool *pool)
    {
        pool->store(pair.first);
        pool->store(pair.second);
    }
    static void load(std::pair<T, U> &pair, PersistentPool *pool)
    {
        pool->load(pair.first);
        pool->load(pair.second);
    }
};

template<typename T> struct PersistentPool::Helper<QFlags<T>>
{
    using Int = typename QFlags<T>::Int;
    static void store(const QFlags<T> &flags, PersistentPool *pool)
    {
        pool->store<Int>(flags);
    }
    static void load(QFlags<T> &flags, PersistentPool *pool)
    {
        flags = QFlags<T>(pool->load<Int>());
    }
};

template<typename T> struct IsSimpleContainer : std::false_type { };
template<> struct IsSimpleContainer<QStringList> : std::true_type { };
template<typename T> struct IsSimpleContainer<QList<T>> : std::true_type { };
template<typename T> struct IsSimpleContainer<std::vector<T>> : std::true_type { };

template<typename T> struct PersistentPool::Helper<T, std::enable_if_t<IsSimpleContainer<T>::value>>
{
    static void store(const T &container, PersistentPool *pool)
    {
        pool->store(int(container.size()));
        for (auto it = container.cbegin(); it != container.cend(); ++it)
            pool->store(*it);
    }
    static void load(T &container, PersistentPool *pool)
    {
        const int count = pool->load<int>();
        container.clear();
        container.reserve(count);
        for (int i = count; --i >= 0;)
            container.push_back(pool->load<typename T::value_type>());
    }
};

template<typename T> struct IsKeyValueContainer : std::false_type { };
template<typename K, typename V> struct IsKeyValueContainer<QMap<K, V>> : std::true_type { };
template<typename K, typename V> struct IsKeyValueContainer<QHash<K, V>> : std::true_type { };

template<typename T>
struct PersistentPool::Helper<T, std::enable_if_t<IsKeyValueContainer<T>::value>>
{
    static void store(const T &container, PersistentPool *pool)
    {
        pool->store(container.size());
        for (auto it = container.cbegin(); it != container.cend(); ++it) {
            pool->store(it.key());
            pool->store(it.value());
        }
    }
    static void load(T &container, PersistentPool *pool)
    {
        container.clear();
        const int count = pool->load<int>();
        for (int i = 0; i < count; ++i) {
            const auto &key = pool->load<typename T::key_type>();
            const auto &value = pool->load<typename T::mapped_type>();
            container.insert(key, value);
        }
    }
};

template<typename K, typename V, typename H>
struct PersistentPool::Helper<std::unordered_map<K, V, H>>
{
    static void store(const std::unordered_map<K, V, H> &map, PersistentPool *pool)
    {
        pool->store(quint32(map.size()));
        for (auto it = map.cbegin(); it != map.cend(); ++it)
            pool->store(*it);
    }
    static void load(std::unordered_map<K, V, H> &map, PersistentPool *pool)
    {
        map.clear();
        const auto count = pool->load<quint32>();
        for (std::size_t i = 0; i < count; ++i)
            map.insert(pool->load<std::pair<K, V>>());
    }
};

} // namespace Internal
} // namespace qbs

#endif
