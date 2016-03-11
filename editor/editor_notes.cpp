#include "editor/editor_notes.hpp"

#include "platform/platform.hpp"

#include "coding/internal/file_data.hpp"

#include "geometry/mercator.hpp"

#include "base/string_utils.hpp"
#include "base/assert.hpp"
#include "base/logging.hpp"
#include "base/timer.hpp"

#include "std/future.hpp"

#include "3party/pugixml/src/pugixml.hpp"

namespace
{
bool LoadFromXml(pugi::xml_document const & xml,
                 vector<editor::Note> & notes,
                 uint32_t & uploadedNotesCount)
{
  uint64_t notesCount;
  auto const root = xml.child("notes");
  if (!strings::to_uint64(root.attribute("count").value(), notesCount))
    return false;
  uploadedNotesCount = static_cast<uint32_t>(notesCount);
  for (auto const xNode : root.select_nodes("note"))
  {
    m2::PointD point;

    auto const node = xNode.node();
    auto const point_x = node.attribute("x");
    if (!point_x || !strings::to_double(point_x.value(), point.x))
      return false;

    auto const point_y = node.attribute("y");
    if (!point_y || !strings::to_double(point_y.value(), point.y))
      return false;

    auto const text = node.attribute("text");
    if (!text)
      return false;

    notes.emplace_back(point, text.value());
  }
  return true;
}

void SaveToXml(vector<editor::Note> const & notes,
               pugi::xml_document & xml,
               uint32_t const UploadedNotesCount)
{
  auto root = xml.append_child("notes");
  root.append_attribute("count") = UploadedNotesCount;
  for (auto const & note : notes)
  {
    auto node = root.append_child("note");
    node.append_attribute("x") = DebugPrint(note.m_point.x).data();
    node.append_attribute("y") = DebugPrint(note.m_point.y).data();
    node.append_attribute("text") = note.m_note.data();
  }
}

vector<editor::Note> MoveNoteVectorAtomicly(vector<editor::Note> && notes, mutex & mu)
{
  lock_guard<mutex> g(mu);
  return move(notes);
}
}  // namespace

namespace editor
{
shared_ptr<Notes> Notes::MakeNotes(string const & fileName)
{
  return shared_ptr<Notes>(new Notes(fileName));
}

Notes::Notes(string const & fileName)
    : m_fileName(fileName)
{
  Load();
}

void Notes::CreateNote(m2::PointD const & point, string const & text)
{
  lock_guard<mutex> g(m_mu);
  m_notes.emplace_back(point, text);
  Save();
}

void Notes::Upload(osm::OsmOAuth const & auth)
{
  auto const launch = [this, &auth]()
  {
    // Capture self to keep it from destruction until this thread is done.
    auto const self = shared_from_this();
    return async(launch::async,
                 [self, auth]()
                 {
                   auto const notes = MoveNoteVectorAtomicly(move(self->m_notes),
                                                             self->m_mu);

                   vector<Note> unuploaded;
                   osm::ServerApi06 api(auth);
                   for (auto const & note : notes)
                   {
                     try
                     {
                       api.CreateNote(MercatorBounds::ToLatLon(note.m_point), note.m_note);
                       ++self->m_uploadedNotes;
                     }
                     catch (osm::ServerApi06::ServerApi06Exception const & e)
                     {
                       LOG(LERROR, ("Can't upload note.", e.Msg()));
                       unuploaded.push_back(note);
                     }
                   }

                   lock_guard<mutex> g(self->m_mu);
                   self->m_notes.insert(end(self->m_notes), begin(unuploaded), end(unuploaded));
                 });
  };

  // Do not run more than one upload thread at a time.
  static auto future = launch();
  auto const status = future.wait_for(milliseconds(0));
  if (status == future_status::ready)
    future = launch();
}

bool Notes::Load()
{
  string content;
  try
  {
    auto const reader = GetPlatform().GetReader(m_fileName);
    reader->ReadAsString(content);
  }
  catch (FileAbsentException const &)
  {
    LOG(LINFO, ("No edits file."));
    return true;
  }
  catch (Reader::Exception const &)
  {
    LOG(LERROR, ("Can't process file.", m_fileName));
    return false;
  }

  pugi::xml_document xml;
  if (!xml.load_buffer(content.data(), content.size()))
  {
    LOG(LERROR, ("Can't load notes, xml is illformed."));
    return false;
  }

  lock_guard<mutex> g(m_mu);
  m_notes.clear();
  if (!LoadFromXml(xml, m_notes, m_uploadedNotes))
  {
    LOG(LERROR, ("Can't load notes, file is illformed."));
    return false;
  }

  return true;
}

/// Not thread-safe, use syncronization.
bool Notes::Save()
{
  pugi::xml_document xml;
  SaveToXml(m_notes, xml, m_uploadedNotes);

  string const tmpFileName = m_fileName + ".tmp";
  if (!xml.save_file(tmpFileName.data(), "  "))
  {
    LOG(LERROR, ("Can't save map edits into", tmpFileName));
    return false;
  }
  else if (!my::RenameFileX(tmpFileName, m_fileName))
  {
    LOG(LERROR, ("Can't rename file", tmpFileName, "to", m_fileName));
    return false;
  }
  return true;
}
}
