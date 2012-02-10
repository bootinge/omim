#include "search_query.hpp"
#include "category_info.hpp"
#include "feature_offset_match.hpp"
#include "lang_keywords_scorer.hpp"
#include "latlon_match.hpp"
#include "search_common.hpp"

#include "../indexer/feature_covering.hpp"
#include "../indexer/features_vector.hpp"
#include "../indexer/index.hpp"
#include "../indexer/scales.hpp"
#include "../indexer/search_delimiters.hpp"
#include "../indexer/search_string_utils.hpp"

#include "../coding/multilang_utf8_string.hpp"

#include "../base/logging.hpp"
#include "../base/string_utils.hpp"
#include "../base/stl_add.hpp"

#include "../std/algorithm.hpp"
#include "../std/array.hpp"
#include "../std/bind.hpp"


namespace search
{

Query::Query(Index const * pIndex,
             CategoriesMapT const * pCategories,
             StringsToSuggestVectorT const * pStringsToSuggest,
             storage::CountryInfoGetter const * pInfoGetter)
  : m_pIndex(pIndex),
    m_pCategories(pCategories),
    m_pStringsToSuggest(pStringsToSuggest),
    m_pInfoGetter(pInfoGetter),
    m_preferredLanguage(StringUtf8Multilang::GetLangIndex("en")),
    m_viewport(m2::RectD::GetEmptyRect()), m_viewportExtended(m2::RectD::GetEmptyRect()),
    m_position(empty_pos_value, empty_pos_value),
    m_bOffsetsCacheIsValid(false)
{
}

Query::~Query()
{
}

void Query::SetViewport(m2::RectD const & viewport)
{
  // TODO: Set m_bOffsetsCacheIsValid = false when mwm index is added or removed!

  if (m_viewport != viewport || !m_bOffsetsCacheIsValid)
  {
    m_viewport = viewport;
    m_viewportExtended = m_viewport;
    m_viewportExtended.Scale(3);

    UpdateViewportOffsets();
  }
}

void Query::SetPreferredLanguage(string const & lang)
{
  m_preferredLanguage = StringUtf8Multilang::GetLangIndex(lang);
}

void Query::ClearCache()
{
  m_offsetsInViewport.clear();
  m_bOffsetsCacheIsValid = false;
}

void Query::UpdateViewportOffsets()
{
  m_offsetsInViewport.clear();

  vector<MwmInfo> mwmInfo;
  m_pIndex->GetMwmInfo(mwmInfo);
  m_offsetsInViewport.resize(mwmInfo.size());

  int const viewScale = scales::GetScaleLevel(m_viewport);
  covering::CoveringGetter cov(m_viewport, 0);

  for (MwmSet::MwmId mwmId = 0; mwmId < mwmInfo.size(); ++mwmId)
  {
    // Search only mwms that intersect with viewport (world always does).
    if (m_viewportExtended.IsIntersect(mwmInfo[mwmId].m_limitRect))
    {
      Index::MwmLock mwmLock(*m_pIndex, mwmId);
      if (MwmValue * pMwm = mwmLock.GetValue())
      {
        feature::DataHeader const & header = pMwm->GetHeader();
        if (header.GetType() == feature::DataHeader::country)
        {
          pair<int, int> const scaleR = header.GetScaleRange();
          int const scale = min(max(viewScale + 7, scaleR.first), scaleR.second);

          covering::IntervalsT const & interval = cov.Get(header.GetLastScale());

          ScaleIndex<ModelReaderPtr> index(pMwm->m_cont.GetReader(INDEX_FILE_TAG),
                                           pMwm->m_factory);

          for (size_t i = 0; i < interval.size(); ++i)
          {
            index.ForEachInIntervalAndScale(MakeBackInsertFunctor(m_offsetsInViewport[mwmId]),
                                            interval[i].first, interval[i].second,
                                            scale);
          }

          sort(m_offsetsInViewport[mwmId].begin(), m_offsetsInViewport[mwmId].end());
        }
      }
    }
  }

  m_bOffsetsCacheIsValid = true;

#ifdef DEBUG
  size_t offsetsCached = 0;
  for (MwmSet::MwmId mwmId = 0; mwmId < mwmInfo.size(); ++mwmId)
    offsetsCached += m_offsetsInViewport[mwmId].size();

  LOG(LDEBUG, ("For search in viewport cached ",
              "mwms:", mwmInfo.size(),
              "offsets:", offsetsCached));
#endif
}

namespace
{
  typedef bool (*CompareFunctionT1) (impl::PreResult1 const &, impl::PreResult1 const &);
  typedef bool (*CompareFunctionT2) (impl::PreResult2 const &, impl::PreResult2 const &);

  CompareFunctionT1 g_arrCompare1[] =
  {
    &impl::PreResult1::LessRank,
    &impl::PreResult1::LessViewportDistance,
    &impl::PreResult1::LessDistance
  };

  CompareFunctionT2 g_arrCompare2[] =
  {
    &impl::PreResult2::LessRank,
    &impl::PreResult2::LessViewportDistance,
    &impl::PreResult2::LessDistance
  };
}

void Query::Search(string const & query, Results & res, unsigned int resultsNeeded)
{
  // Initialize.
  {
    m_cancel = false;
    m_rawQuery = query;
    m_uniQuery = NormalizeAndSimplifyString(m_rawQuery);
    m_tokens.clear();
    m_prefix.clear();

    search::Delimiters delims;
    SplitUniString(m_uniQuery, MakeBackInsertFunctor(m_tokens), delims);

    if (!m_tokens.empty() && !delims(strings::LastUniChar(m_rawQuery)))
    {
      m_prefix.swap(m_tokens.back());
      m_tokens.pop_back();
    }
    if (m_tokens.size() > 31)
      m_tokens.resize(31);

    vector<vector<int8_t> > langPriorities(3);
    langPriorities[0].push_back(m_preferredLanguage);
    langPriorities[1].push_back(StringUtf8Multilang::GetLangIndex("int_name"));
    langPriorities[1].push_back(StringUtf8Multilang::GetLangIndex("en"));
    langPriorities[2].push_back(StringUtf8Multilang::GetLangIndex("default"));
    m_pKeywordsScorer.reset(new LangKeywordsScorer(langPriorities,
                                                   m_tokens.data(), m_tokens.size(), &m_prefix));

    // Results queue's initialization.
    STATIC_ASSERT ( m_qCount == ARRAY_SIZE(g_arrCompare1) );
    STATIC_ASSERT ( m_qCount == ARRAY_SIZE(g_arrCompare2) );

    for (size_t i = 0; i < m_qCount; ++i)
    {
      m_results[i] = QueueT(2 * resultsNeeded, QueueCompareT(g_arrCompare1[i]));
      m_results[i].reserve(2 * resultsNeeded);
    }
  }

  // Match (lat, lon).
  {
    double lat, lon, latPrec, lonPrec;
    if (search::MatchLatLon(m_rawQuery, lat, lon, latPrec, lonPrec))
    {
      //double const precision = 5.0 * max(0.0001, min(latPrec, lonPrec));  // Min 55 meters
      res.AddResult(impl::PreResult2(m_viewport, m_position, lat, lon).
                    GenerateFinalResult(m_pInfoGetter, m_pCategories));
    }
  }

  if (m_cancel) return;
  SuggestStrings(res);

  if (m_cancel) return;
  SearchFeatures();

  if (m_cancel) return;
  FlushResults(res);
}

namespace
{
  /// @name Functors to convert pointers to referencies.
  /// Pass them to stl algorithms.
  //@{
  template <class FunctorT> class ProxyFunctor1
  {
    FunctorT m_fn;
  public:
    template <class T> explicit ProxyFunctor1(T const & p) : m_fn(*p) {}
    template <class T> bool operator() (T const & p) { return m_fn(*p); }
  };

  template <class FunctorT> class ProxyFunctor2
  {
    FunctorT m_fn;
  public:
    template <class T> bool operator() (T const & p1, T const & p2)
    {
      return m_fn(*p1, *p2);
    }
  };
  //@}

  class IndexedValue
  {
  public:
     typedef impl::PreResult2 value_type;

  private:
    array<size_t, Query::m_qCount> m_ind;

    /// @todo Do not use shared_ptr for optimization issues.
    /// Need to rewrite std::unique algorithm.
    shared_ptr<value_type> m_val;

  public:
    explicit IndexedValue(value_type * v) : m_val(v)
    {
      for (size_t i = 0; i < m_ind.size(); ++i)
        m_ind[i] = numeric_limits<size_t>::max();
    }

    value_type const & operator*() const { return *m_val; }

    void SetIndex(size_t i, size_t v) { m_ind[i] = v; }

    void SortIndex()
    {
      sort(m_ind.begin(), m_ind.end());
    }

    string DebugPrint() const
    {
      string index;
      for (size_t i = 0; i < m_ind.size(); ++i)
        index = index + " " + strings::to_string(m_ind[i]);

      return impl::DebugPrint(*m_val) + "; Index:" + index;
    }

    bool operator < (IndexedValue const & r) const
    {
      for (size_t i = 0; i < m_ind.size(); ++i)
      {
        if (m_ind[i] != r.m_ind[i])
          return (m_ind[i] < r.m_ind[i]);
      }

      return false;
    }
  };

  inline string DebugPrint(IndexedValue const & t)
  {
    return t.DebugPrint();
  }

  struct LessByFeatureID
  {
    typedef impl::PreResult1 ValueT;
    bool operator() (ValueT const & r1, ValueT const & r2) const
    {
      return (r1.GetID() < r2.GetID());
    }
  };
}

namespace impl
{
  class PreResult2Maker
  {
    typedef map<size_t, FeaturesVector *> FeaturesMapT;
    FeaturesMapT m_features;

    vector<MwmInfo> m_mwmInfo;

    Query & m_query;

  public:
    PreResult2Maker(Query & q) : m_query(q)
    {
      m_query.m_pIndex->GetMwmInfo(m_mwmInfo);
    }
    ~PreResult2Maker()
    {
      for (FeaturesMapT::iterator i = m_features.begin(); i != m_features.end(); ++i)
        delete i->second;
    }

    impl::PreResult2 * operator() (impl::PreResult1 const & r)
    {
      // Find or create needed FeaturesVector for result.
      pair<uint32_t, size_t> const id = r.GetID();
      string countryName;

      FeaturesMapT::iterator iF = m_features.insert(
            make_pair(id.second, static_cast<FeaturesVector*>(0))).first;
      if (iF->second == 0)
      {
        for (MwmSet::MwmId mwmId = 0; mwmId < m_mwmInfo.size(); ++mwmId)
        {
          if (mwmId == id.second)
          {
            Index::MwmLock mwmLock(*m_query.m_pIndex, mwmId);
            countryName = mwmLock.GetCountryName();

            if (MwmValue * pMwm = mwmLock.GetValue())
            {
              feature::DataHeader const & h = pMwm->GetHeader();
              if (h.GetType() == feature::DataHeader::world)
                countryName = string();

              iF->second = new FeaturesVector(pMwm->m_cont, h);
              break;
            }
          }
        }
      }

      if (iF->second == 0)
      {
        LOG(LERROR, ("Valid MWM for search result not found", id));
        return 0;
      }

      FeatureType feature;
      iF->second->Get(id.first, feature);

      uint32_t penalty;
      string name;
      m_query.GetBestMatchName(feature, penalty, name);

      return new impl::PreResult2(feature, r, name, countryName);
    }
  };
}

void Query::FlushResults(Results & res)
{
  typedef impl::PreResult2 ResultT;

  /*
  #ifdef DEBUG
      {
        impl::PreResult2Maker maker(*this);
        LOG(LDEBUG, ("Dump features for rank:"));
        for (QueueT::const_iterator i = m_results[0].begin(); i != m_results[0].end(); ++i)
        {
          ResultT * res = maker(*i);
          LOG(LDEBUG, (*res));
          delete res;
        }
        LOG(LDEBUG, ("------------------------"));
      }
  #endif
  */

  vector<IndexedValue> indV;

  {
    // make unique set of PreResult1
    typedef set<impl::PreResult1, LessByFeatureID> PreResultSetT;
    PreResultSetT theSet;

    for (size_t i = 0; i < m_qCount; ++i)
    {
      theSet.insert(m_results[i].begin(), m_results[i].end());
      m_results[i].clear();
    }

    // make PreResult2 vector
    impl::PreResult2Maker maker(*this);
    for (PreResultSetT::const_iterator i = theSet.begin(); i != theSet.end(); ++i)
    {
      ResultT * res = maker(*i);
      if (res == 0) continue;

      // do not insert duplicating results
      if (indV.end() == find_if(indV.begin(), indV.end(), ProxyFunctor1<ResultT::StrictEqualF>(res)))
        indV.push_back(IndexedValue(res));
      else
        delete res;
    }
  }

  // remove duplicating linear objects
  sort(indV.begin(), indV.end(), ProxyFunctor2<ResultT::LessLinearTypesF>());
  indV.erase(unique(indV.begin(), indV.end(),
                    ProxyFunctor2<ResultT::EqualLinearTypesF>()),
             indV.end());

  for (size_t i = 0; i < m_qCount; ++i)
  {
    CompareT<ResultT, RefSmartPtr> comp(g_arrCompare2[i]);

    // sort by needed criteria
    sort(indV.begin(), indV.end(), comp);

    // assign ranks
    size_t rank = 0;
    for (size_t j = 0; j < indV.size(); ++j)
    {
      if (j > 0 && comp(indV[j-1], indV[j]))
        ++rank;

      indV[j].SetIndex(i, rank);
    }
  }

  // prepare combined criteria
  for_each(indV.begin(), indV.end(), bind(&IndexedValue::SortIndex, _1));

  // sort results according to combined criteria
  sort(indV.begin(), indV.end());

  // emit feature results
  for (size_t i = 0; i < indV.size(); ++i)
  {
    if (m_cancel) break;

    LOG(LDEBUG, (indV[i]));

    res.AddResult((*(indV[i])).GenerateFinalResult(m_pInfoGetter, m_pCategories));
  }
}

namespace
{
  class EqualFeature
  {
    typedef impl::PreResult1 ValueT;
    ValueT const & m_val;

  public:
    EqualFeature(ValueT const & v) : m_val(v) {}
    bool operator() (ValueT const & r) const
    {
      return (m_val.GetID() == r.GetID());
    }
  };
}

void Query::AddResultFromTrie(TrieValueT const & val, size_t mwmID)
{
  impl::PreResult1 res(val.m_featureId, val.m_rank, val.m_pt, mwmID, m_position, m_viewport);

  for (size_t i = 0; i < m_qCount; ++i)
  {
    // here can be the duplicates because of different language match (for suggest token)
    if (m_results[i].end() == find_if(m_results[i].begin(), m_results[i].end(), EqualFeature(res)))
      m_results[i].push(res);
  }
}

namespace impl
{

class BestNameFinder
{
  uint32_t & m_penalty;
  string & m_name;
  LangKeywordsScorer & m_keywordsScorer;
public:
  BestNameFinder(uint32_t & penalty, string & name, LangKeywordsScorer & keywordsScorer)
    : m_penalty(penalty), m_name(name), m_keywordsScorer(keywordsScorer)
  {
    m_penalty = uint32_t(-1);
  }

  bool operator()(signed char lang, string const & name) const
  {
    uint32_t penalty = m_keywordsScorer.Score(lang, name);
    if (penalty < m_penalty)
    {
      m_penalty = penalty;
      m_name = name;
    }
    return true;
  }
};

}  // namespace search::impl

void Query::GetBestMatchName(FeatureType const & f, uint32_t & penalty, string & name)
{
  impl::BestNameFinder bestNameFinder(penalty, name, *m_pKeywordsScorer);
  (void)f.ForEachNameRef(bestNameFinder);
  /*
  if (!f.ForEachNameRef(bestNameFinder))
  {
    feature::TypesHolder types(f);
    LOG(LDEBUG, (types));
    LOG(LDEBUG, (f.GetLimitRect(-1)));
  }
  */
}

namespace impl
{

class FeatureLoader
{
  Query & m_query;
  size_t m_mwmID;
  size_t m_count;

public:
  FeatureLoader(Query & query, size_t mwmID)
    : m_query(query), m_mwmID(mwmID), m_count(0)
  {
  }

  void operator() (Query::TrieValueT const & value)
  {
    ++m_count;
    m_query.AddResultFromTrie(value, m_mwmID);
  }

  size_t GetCount() const { return m_count; }
  void Reset() { m_count = 0; }
};

}  // namespace search::impl

void Query::SearchFeatures()
{
  if (!m_pIndex)
    return;

  vector<vector<strings::UniString> > tokens(m_tokens.size());

  // Add normal tokens.
  for (size_t i = 0; i < m_tokens.size(); ++i)
    tokens[i].push_back(m_tokens[i]);

  // Add names of categories.
  if (m_pCategories)
  {
    for (size_t i = 0; i < m_tokens.size(); ++i)
    {
      typedef CategoriesMapT::const_iterator IterT;

      pair<IterT, IterT> const range = m_pCategories->equal_range(m_tokens[i]);
      for (IterT it = range.first; it != range.second; ++it)
        tokens[i].push_back(FeatureTypeToString(it->second));
    }
  }

  vector<MwmInfo> mwmInfo;
  m_pIndex->GetMwmInfo(mwmInfo);

  unordered_set<int8_t> langs;
  langs.insert(m_preferredLanguage);
  langs.insert(StringUtf8Multilang::GetLangIndex("int_name"));
  langs.insert(StringUtf8Multilang::GetLangIndex("en"));
  langs.insert(StringUtf8Multilang::GetLangIndex("default"));

  SearchFeatures(tokens, mwmInfo, langs, true);
}

namespace
{
  class FeaturesFilter
  {
    vector<uint32_t> const & m_offsets;
    bool m_alwaysTrue;

    volatile bool & m_isCancel;
  public:
    FeaturesFilter(vector<uint32_t> const & offsets, bool alwaysTrue, volatile bool & isCancel)
      : m_offsets(offsets), m_alwaysTrue(alwaysTrue), m_isCancel(isCancel)
    {
    }

    bool operator() (uint32_t offset) const
    {
      if (m_isCancel)
        throw Query::CancelException();

      return (m_alwaysTrue || binary_search(m_offsets.begin(), m_offsets.end(), offset));
    }
  };
}

void Query::SearchFeatures(vector<vector<strings::UniString> > const & tokens,
                           vector<MwmInfo> const & mwmInfo,
                           unordered_set<int8_t> const & langs,
                           bool onlyInViewport)
{
  for (MwmSet::MwmId mwmId = 0; mwmId < mwmInfo.size(); ++mwmId)
  {
    // Search only mwms that intersect with viewport (world always does).
    if (!onlyInViewport ||
        m_viewportExtended.IsIntersect(mwmInfo[mwmId].m_limitRect))
    {
      Index::MwmLock mwmLock(*m_pIndex, mwmId);
      if (MwmValue * pMwm = mwmLock.GetValue())
      {
        if (pMwm->m_cont.IsReaderExist(SEARCH_INDEX_FILE_TAG))
        {
          feature::DataHeader const & header = pMwm->GetHeader();
          serial::CodingParams cp(GetCPForTrie(header.GetDefCodingParams()));

          scoped_ptr<TrieIterator> pTrieRoot(::trie::reader::ReadTrie(
                                               pMwm->m_cont.GetReader(SEARCH_INDEX_FILE_TAG),
                                               trie::ValueReader(cp),
                                               trie::EdgeValueReader()));

          // Get categories edge root.
          scoped_ptr<TrieIterator> pCategoriesRoot;
          TrieIterator::Edge::EdgeStrT categoriesEdge;

          size_t const count = pTrieRoot->m_edge.size();
          for (size_t i = 0; i < count; ++i)
          {
            TrieIterator::Edge::EdgeStrT const & edge = pTrieRoot->m_edge[i].m_str;
            ASSERT_GREATER_OR_EQUAL(edge.size(), 1, ());

            if (edge[0] == search::CATEGORIES_LANG)
            {
              categoriesEdge = edge;
              pCategoriesRoot.reset(pTrieRoot->GoToEdge(i));
              break;
            }
          }
          ASSERT_NOT_EQUAL(pCategoriesRoot, 0, ());

          bool const isWorld = (header.GetType() == feature::DataHeader::world);

          impl::FeatureLoader emitter(*this, mwmId);

          // Iterate through first language edges.
          for (size_t i = 0; i < count; ++i)
          {
            TrieIterator::Edge::EdgeStrT const & edge = pTrieRoot->m_edge[i].m_str;
            if (edge[0] < search::CATEGORIES_LANG && langs.count(static_cast<int8_t>(edge[0])))
            {
              scoped_ptr<TrieIterator> pLangRoot(pTrieRoot->GoToEdge(i));

              MatchFeaturesInTrie(tokens, m_prefix,
                                  TrieRootPrefix(*pLangRoot, edge),
                                  TrieRootPrefix(*pCategoriesRoot, categoriesEdge),
                                  FeaturesFilter(m_offsetsInViewport[mwmId], isWorld, m_cancel), emitter);

              LOG(LDEBUG, ("Lang:",
                           StringUtf8Multilang::GetLangByCode(static_cast<int8_t>(edge[0])),
                           "Matched: ",
                           emitter.GetCount()));

              emitter.Reset();
            }
          }
        }
      }
    }
  }
}

void Query::SuggestStrings(Results & res)
{
  if (m_pStringsToSuggest)
  {
    if (m_tokens.size() == 0 && !m_prefix.empty())
    {
      // Match prefix.
      MatchForSuggestions(m_prefix, res);
    }
    else if (m_tokens.size() == 1)
    {
      // Match token + prefix.
      strings::UniString tokenAndPrefix = m_tokens[0];
      if (!m_prefix.empty())
      {
        tokenAndPrefix.push_back(' ');
        tokenAndPrefix.append(m_prefix.begin(), m_prefix.end());
      }

      MatchForSuggestions(tokenAndPrefix, res);
    }
  }
}

void Query::MatchForSuggestions(strings::UniString const & token, Results & res)
{
  StringsToSuggestVectorT::const_iterator it = m_pStringsToSuggest->begin();
  for (; it != m_pStringsToSuggest->end(); ++it)
  {
    strings::UniString const & s = it->first;
    if (it->second <= token.size() && StartsWith(s.begin(), s.end(), token.begin(), token.end()))
      res.AddResult(impl::PreResult2(strings::ToUtf8(s), it->second).
                    GenerateFinalResult(m_pInfoGetter, m_pCategories));
  }
}

}  // namespace search
