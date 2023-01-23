# frozen_string_literal: true

require_relative 'helper'

require "active_record"
require "active_record/connection_adapters/extralite_adapter"

class T < ::ActiveRecord::Base
  self.table_name = "t"
  self.primary_key = "id"

  validates :t, presence: true
end

class ActiveRecordTest < MiniTest::Test
  def test_extralite_connection
    db = ActiveRecord::Base.extralite_connection(adapter: "extralite", database: ":memory:", prepared_statements: false);
    db.create_table("t", id: false, force: false, primary_key: "id") do |t|
      t.column :id, :int
      t.column :t, :text
    end
    cols = db.columns("t")
    assert_equal cols.size, 2
    db.exec_query("INSERT INTO t VALUES (1,'t'), (2, 'f')")
    assert_equal db.exec_query("SELECT * FROM t WHERE id = ? OR id = ?", "fake_name", [
      ActiveRecord::Relation::QueryAttribute.new("id", 1, ActiveRecord::Type::Integer.new),
      ActiveRecord::Relation::QueryAttribute.new("id", 3, ActiveRecord::Type::Integer.new)
    ]).rows, [{:id=>1, :t=>"t"}]
    db.disconnect!
  end

  def test_ar_establish
    ActiveRecord::Base.establish_connection({
      :adapter => 'extralite',
      :database => ':memory:',
    })

    db = ActiveRecord::Base.connection
    db.create_table("t", id: false, force: false, primary_key: "id") do |t|
      t.column :id, :int
      t.column :t, :text
    end

    # create
    T.create(id: 1, t: "t")

    # update
    c = T.create(id: 3, t: "f")
    c.id = 2
    c.save

    # read
    puts db.exec_query("SELECT * FROM t")

    puts ">- #{T.select("t").first.t}"
    puts ">> #{T.all.first.t}"

    assert_equal T.all, [T.new(id: 1, t: "t"), T.new(id: 2, t: "f")]
    assert_equal T.find_by(id: 2), T.new(id: 2, t: "f")

    db.disconnect!
  end
end
