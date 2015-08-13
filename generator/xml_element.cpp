#include "generator/xml_element.hpp"

#include "coding/parse_xml.hpp"

#include "std/cstdio.hpp"
#include "std/algorithm.hpp"


void XMLElement::AddKV(string const & k, string const & v)
{
  childs.push_back(XMLElement());
  XMLElement & e = childs.back();

  e.type = EntityType::Tag;
  e.k = k;
  e.v = v;
}

void XMLElement::AddND(uint64_t ref)
{
  m_nds.push_back(ref);
}

void XMLElement::AddMEMBER(uint64_t ref, EntityType type, string const & role)
{
  m_members.push_back({ref, type, role});
}

string DebugPrint(XMLElement::EntityType e)
{
  switch (e)
  {
    case XMLElement::EntityType::Unknown:
      return "Unknown";
    case XMLElement::EntityType::Way:
      return "Way";
    case XMLElement::EntityType::Tag:
      return "Tag";
    case XMLElement::EntityType::Relation:
      return "Relation";
    case XMLElement::EntityType::Osm:
      return "Osm";
    case XMLElement::EntityType::Node:
      return "Node";
    case XMLElement::EntityType::Nd:
      return "Nd";
    case XMLElement::EntityType::Member:
      return "Member";
  }
}

string XMLElement::ToString(string const & shift) const
{
  stringstream ss;
  ss << (shift.empty() ? "\n" : shift);
  switch (type)
  {
    case EntityType::Node:
      ss << "Node: " << id << " (" << fixed << setw(7) << lat << ", " << lon << ")" << " subelements: " << childs.size();
      break;
    case EntityType::Nd:
      ss << "Nd ref: " << ref;
      break;
    case EntityType::Way:
      ss << "Way: " << id << " nds: " << m_nds.size() << " subelements: " << childs.size();
      if (!m_nds.empty())
      {
        string shift2 = shift;
        shift2 += shift2.empty() ? "\n  " : "  ";
        for ( auto const & e : m_nds )
          ss << shift2 << e;
      }
      break;
    case EntityType::Relation:
      ss << "Relation: " << id << " members: " << m_members.size() << " subelements: " << childs.size();
      if (!m_members.empty())
      {
        string shift2 = shift;
        shift2 += shift2.empty() ? "\n  " : "  ";
        for ( auto const & e : m_members )
          ss << shift2 << e.ref << " " << DebugPrint(e.type) << " " << e.role;
      }
      break;
    case EntityType::Tag:
      ss << "Tag: " << k << " = " << v;
      break;
    case EntityType::Member:
      ss << "Member: " << ref << " type: " << DebugPrint(memberType) << " role: " << role;
      break;
    default:
      ss << "Unknown element";
  }
  if (!childs.empty())
  {
    string shift2 = shift;
    shift2 += shift2.empty() ? "\n  " : "  ";
    for ( auto const & e : childs )
      ss << e.ToString(shift2);
  }
  return ss.str();
}


string DebugPrint(XMLElement const & e)
{
  return e.ToString();
}
