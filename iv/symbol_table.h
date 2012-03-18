#ifndef IV_SYMBOLTABLE_H_
#define IV_SYMBOLTABLE_H_
#include <string>
#include <vector>
#include <iv/detail/unordered_map.h>
#include <iv/ustring.h>
#include <iv/conversions.h>
#include <iv/symbol.h>
namespace iv {
namespace core {

class SymbolTable {
 public:
  class SymbolHolder {
   public:
    SymbolHolder(const UStringPiece& piece)
      : rep_(),
        size_(piece.size()),
        is_8bit_(false),
        pointer_(NULL) {
      rep_.rep16_ = piece.data();
    }

    SymbolHolder(const StringPiece& piece)
      : rep_(),
        size_(piece.size()),
        is_8bit_(true),
        pointer_(NULL) {
      rep_.rep8_ = piece.data();
    }

    SymbolHolder(const UString* str)
      : rep_(),
        size_(str->size()),
        is_8bit_(false),
        pointer_(str),
        hash_(Hash::StringToHash(*str)) {
      rep_.rep16_ = str->data();
    }

    std::size_t hash() const {
      if (pointer_) {
        return hash_;
      }
      if (is_8bit_) {
        return Hash::StringToHash(StringPiece(rep_.rep8_, size_));
      } else {
        return Hash::StringToHash(UStringPiece(rep_.rep16_, size_));
      }
    }

    friend bool operator==(const SymbolHolder& lhs,
                           const SymbolHolder& rhs) {
      if (lhs.is_8bit_ == rhs.is_8bit_) {
        if (lhs.is_8bit_) {
          // 8bits
          return
              StringPiece(lhs.rep_.rep8_, lhs.size_) ==
              StringPiece(rhs.rep_.rep8_, rhs.size_);
        }
        return
            UStringPiece(lhs.rep_.rep16_, lhs.size_) ==
            UStringPiece(rhs.rep_.rep16_, rhs.size_);
      } else {
        if (lhs.size_ != rhs.size_) {
          return false;
        }
        if (lhs.is_8bit_) {
          return CompareIterators(
              lhs.rep_.rep8_, lhs.rep_.rep8_ + lhs.size_,
              rhs.rep_.rep16_, rhs.rep_.rep16_ + rhs.size_) == 0;
        } else {
          return CompareIterators(
              lhs.rep_.rep16_, lhs.rep_.rep16_ + lhs.size_,
              rhs.rep_.rep8_, rhs.rep_.rep8_ + rhs.size_) == 0;
        }
      }
    }

    const UString* pointer() const { return pointer_; }

    std::size_t size() const { return size_; }

   private:
    union Representation {
      const char* rep8_;
      const uint16_t* rep16_;
    } rep_;
    std::size_t size_;
    bool is_8bit_;
    const UString* pointer_;
    std::size_t hash_;
  };

  struct SymbolHolderHasher {
    inline std::size_t operator()(const SymbolHolder& x) const {
      return x.hash();
    }
  };

  typedef std::unordered_set<SymbolHolder, SymbolHolderHasher> Set;

  SymbolTable()
    : set_() {
    // insert default symbols
#define V(sym) InsertDefaults(symbol::sym());
    IV_DEFAULT_SYMBOLS(V)
#undef V
  }

  ~SymbolTable() {
    for (Set::const_iterator it = set_.begin(),
         last = set_.end(); it != last; ++it) {
      const Symbol sym = detail::MakeSymbol(it->pointer());
      if (!symbol::DefaultSymbolProvider::Instance()->IsDefaultSymbol(sym)) {
        delete it->pointer();
      }
    }
  }

  template<class CharT>
  inline Symbol Lookup(const CharT* str) {
    using std::char_traits;
    return Lookup(BasicStringPiece<CharT>(str));
  }

  template<class String>
  inline Symbol Lookup(const String& str) {
    uint32_t index;
    if (ConvertToUInt32(str.begin(), str.end(), &index)) {
      return symbol::MakeSymbolFromIndex(index);
    }
    const SymbolHolder target(str);
    typename Set::const_iterator it = set_.find(target);
    if (it != set_.end()) {
      return detail::MakeSymbol(it->pointer());
    } else {
      const UString* res = new UString(str.begin(), str.end());
      set_.insert(res);
      return detail::MakeSymbol(res);
    }
  }

 private:
  void InsertDefaults(Symbol sym) {
    if (symbol::IsStringSymbol(sym)) {
      assert(set_.find(symbol::GetStringFromSymbol(sym)) == set_.end());
      set_.insert(symbol::GetStringFromSymbol(sym));
    }
  }

  Set set_;
};

} }  // namespace iv::core
#endif  // IV_SYMBOLTABLE_H_