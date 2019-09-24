package yson_test

import (
	"encoding/json"
	"testing"

	"github.com/stretchr/testify/require"

	"a.yandex-team.ru/yt/go/yson"
)

type (
	NullTestVal struct {
		Val int `json:"val,omitempty" yson:"val,omitempty"`
	}

	NullTest struct {
		Val   *NullTestVal      `json:"val,omitempty" yson:"val,omitempty"`
		Int   *int              `json:"int,omitempty" yson:"int,omitempty"`
		Map   map[string]string `json:"map,omitempty" yson:"map,omitempty"`
		Array []string          `json:"array,omitempty" yson:"array,omitempty"`
	}
)

func TestNullHandling(t *testing.T) {
	var jsonTest NullTest
	msgJSON := `{"int": null, "map": null, "array": null}`
	require.NoError(t, json.Unmarshal([]byte(msgJSON), &jsonTest))

	var ysonTest NullTest
	msgYson := `{int=#;map=#;array=#}`
	require.NoError(t, yson.Unmarshal([]byte(msgYson), &ysonTest))

	require.Equal(t, jsonTest, ysonTest)
}

type (
	MyInt      int
	MyUint     uint
	MyIntSlice []int
	MyString   string

	TypedefTest struct {
		I  MyInt
		U  MyUint
		II MyIntSlice
		S  MyString
	}
)

func TestTypedefTypes(t *testing.T) {
	in := TypedefTest{
		I:  1337,
		U:  1234,
		II: []int{1, 3, 3, 7},
		S:  "foobar",
	}

	var out TypedefTest

	js, err := json.Marshal(in)
	require.NoError(t, err)
	require.NoError(t, json.Unmarshal(js, &out))
	require.Equal(t, in, out)

	ys, err := yson.Marshal(in)
	require.NoError(t, err)
	require.NoError(t, yson.Unmarshal(ys, &out))
	require.Equal(t, in, out)
}

func TestMapKeys(t *testing.T) {
	in := map[MyString]int{
		"foo": 1,
		"bar": 2,
	}
	var out map[MyString]int

	ys, err := yson.Marshal(in)
	require.NoError(t, err)
	require.NoError(t, yson.Unmarshal(ys, &out))
	require.Equal(t, in, out)
}

type (
	InnerA struct {
		A int
	}

	InnerB struct {
		B int
	}

	EmbeddingTest struct {
		InnerA
		*InnerB
	}
)

func TestFieldEmbedding(t *testing.T) {
	{
		var in, out EmbeddingTest
		in.A = 10
		in.InnerB = &InnerB{B: 20}

		js, err := json.Marshal(in)
		require.NoError(t, err)
		require.Equal(t, `{"A":10,"B":20}`, string(js))

		require.NoError(t, json.Unmarshal(js, &out))
		require.Equal(t, in, out)
	}

	{
		var in, out EmbeddingTest
		in.A = 10
		in.InnerB = &InnerB{B: 20}

		js, err := yson.MarshalFormat(in, yson.FormatText)
		require.NoError(t, err)
		require.Equal(t, `{A=10;B=20;}`, string(js))

		require.NoError(t, yson.Unmarshal(js, &out))
		require.Equal(t, in, out)

		in.InnerB = nil
		js, err = yson.MarshalFormat(in, yson.FormatText)
		require.NoError(t, err)
		require.Equal(t, `{A=10;}`, string(js))
	}
}

type (
	Unexported struct {
		A int
		b string
		c bool
	}
)

func TestUnexportedFields(t *testing.T) {
	in := Unexported{A: 10, b: "foo", c: true}

	js, err := yson.MarshalFormat(in, yson.FormatText)
	require.NoError(t, err)
	require.Equal(t, `{A=10;}`, string(js))

	var out Unexported
	require.NoError(t, yson.Unmarshal([]byte("{A=10;b=foo}"), &out))

	require.Equal(t, Unexported{A: 10}, out)
}

type (
	RecursiveA struct {
		*RecursiveB
	}

	RecursiveB struct {
		*RecursiveA
	}
)

func TestRecursiveTypes(t *testing.T) {
	t.Skipf("this is broken")

	in := RecursiveA{&RecursiveB{&RecursiveA{&RecursiveB{}}}}
	expected := `{}`

	ys, err := yson.Marshal(in)
	require.NoError(t, err)
	require.Equal(t, expected, string(ys))

	var out RecursiveA
	require.NoError(t, yson.Unmarshal(ys, &out))

	require.Equal(t, in, out)
}

func TestInvalidSkip(t *testing.T) {
	var row struct {
		Name  string
		Value [][]interface{}
	}
	row.Name = "foo"
	row.Value = [][]interface{}{
		{1, 2, 0.1},
		{3, 4, 0.5},
	}

	ys, err := yson.MarshalFormat(row, yson.FormatBinary)
	require.NoError(t, err)

	require.NoError(t, yson.Unmarshal(ys, &row))
}
